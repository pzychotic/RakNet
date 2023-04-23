/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "NativeFeatureIncludes.h"
#if _RAKNET_SUPPORT_NatTypeDetectionServer == 1

#include "NatTypeDetectionServer.h"
#include "SocketIncludes.h"
#include "RakPeerInterface.h"
#include "MessageIdentifiers.h"
#include "GetTime.h"
#include "BitStream.h"

#include <algorithm>

// #define NTDS_VERBOSE

namespace RakNet {

STATIC_FACTORY_DEFINITIONS( NatTypeDetectionServer, NatTypeDetectionServer );

NatTypeDetectionServer::NatTypeDetectionServer()
{
    s1p2 = s2p3 = s3p4 = s4p5 = 0;
}

NatTypeDetectionServer::~NatTypeDetectionServer()
{
    Shutdown();
}

void NatTypeDetectionServer::Startup( const char* nonRakNetIP2, const char* nonRakNetIP3, const char* nonRakNetIP4 )
{
    std::vector<RakNetSocket2*> sockets;
    rakPeerInterface->GetSockets( sockets );
    RakAssert( !sockets.empty() );
    char str[64];
    sockets.front()->GetBoundAddress().ToString( false, str );
    s1p2 = CreateNonblockingBoundSocket( str, this );
    s2p3 = CreateNonblockingBoundSocket( nonRakNetIP2, this );
    s3p4 = CreateNonblockingBoundSocket( nonRakNetIP3, this );
    s4p5 = CreateNonblockingBoundSocket( nonRakNetIP4, this );

    s3p4Address = nonRakNetIP3;

    if( s3p4->IsBerkleySocket() )
    {
        ( (RNS2_Berkley*)s3p4 )->CreateRecvPollingThread( 0 );
    }
}

void NatTypeDetectionServer::Shutdown()
{
    if( s1p2 != 0 )
    {
        RakNet::OP_DELETE( s1p2, _FILE_AND_LINE_ );
        s1p2 = 0;
    }
    if( s2p3 != 0 )
    {
        RakNet::OP_DELETE( s2p3, _FILE_AND_LINE_ );
        s2p3 = 0;
    }
    if( s3p4 != 0 )
    {
        if( s3p4->IsBerkleySocket() )
        {
            ( (RNS2_Berkley*)s3p4 )->BlockOnStopRecvPollingThread();
        }

        RakNet::OP_DELETE( s3p4, _FILE_AND_LINE_ );
        s3p4 = 0;
    }
    if( s4p5 != 0 )
    {
        RakNet::OP_DELETE( s4p5, _FILE_AND_LINE_ );
        s4p5 = 0;
    }
    std::lock_guard<std::mutex> guard( bufferedPacketsMutex );
    for( RNS2RecvStruct* pPacket : bufferedPackets )
    {
        RakNet::OP_DELETE( pPacket, _FILE_AND_LINE_ );
    }
    bufferedPackets.clear();
}

void NatTypeDetectionServer::Update( void )
{
    RakNet::TimeMS time = RakNet::GetTimeMS();
    BitStream bs;

    bufferedPacketsMutex.lock();
    RNS2RecvStruct* recvStruct = nullptr;
    if( !bufferedPackets.empty() )
    {
        recvStruct = bufferedPackets.front();
        bufferedPackets.pop_front();
    }
    bufferedPacketsMutex.unlock();
    while( recvStruct )
    {
        SystemAddress senderAddr = recvStruct->systemAddress;
        char* data = recvStruct->data;
        if( data[0] == NAT_TYPE_PORT_RESTRICTED && recvStruct->socket == s3p4 )
        {
            BitStream bsIn( (unsigned char*)data, recvStruct->bytesRead, false );
            RakNetGUID senderGuid;
            bsIn.IgnoreBytes( sizeof( MessageID ) );
            bool readSuccess = bsIn.Read( senderGuid );
            RakAssert( readSuccess );
            if( readSuccess )
            {
                auto it = std::find_if( natDetectionAttempts.begin(), natDetectionAttempts.end(),
                                        [&senderGuid]( const NATDetectionAttempt& rAttempt ) { return rAttempt.guid == senderGuid; } );
                if( it != natDetectionAttempts.end() )
                {
                    bs.Reset();
                    bs.Write( (unsigned char)ID_NAT_TYPE_DETECTION_RESULT );
                    // If different, then symmetric
                    if( senderAddr != it->systemAddress )
                    {

#ifdef NTDS_VERBOSE
                        printf( "Determined client is symmetric\n" );
#endif
                        bs.Write( (unsigned char)NAT_TYPE_SYMMETRIC );
                    }
                    else
                    {
                        // else port restricted
#ifdef NTDS_VERBOSE

                        printf( "Determined client is port restricted\n" );
#endif
                        bs.Write( (unsigned char)NAT_TYPE_PORT_RESTRICTED );
                    }

                    rakPeerInterface->Send( &bs, HIGH_PRIORITY, RELIABLE, 0, it->systemAddress, false );

                    // Done
                    natDetectionAttempts.erase( it );
                }
                else
                {
                    //RakAssert("i==0 in Update when looking up GUID in NatTypeDetectionServer.cpp. Either a bug or a late resend" && 0);
                }
            }
            else
            {
                //RakAssert("Didn't read GUID in Update in NatTypeDetectionServer.cpp. Message format error" && 0);
            }
        }

        DeallocRNS2RecvStruct( recvStruct, _FILE_AND_LINE_ );

        std::lock_guard<std::mutex> guard( bufferedPacketsMutex );
        if( !bufferedPackets.empty() )
        {
            recvStruct = bufferedPackets.front();
            bufferedPackets.pop_front();
        }
    }

    int i = 0;
    while( i < (int)natDetectionAttempts.size() )
    {
        if( time > natDetectionAttempts[i].nextStateTime )
        {
            RNS2_SendParameters bsp;
            natDetectionAttempts[i].detectionState = (NATDetectionState)( (int)natDetectionAttempts[i].detectionState + 1 );
            natDetectionAttempts[i].nextStateTime = time + natDetectionAttempts[i].timeBetweenAttempts;
            SystemAddress saOut;
            unsigned char c;
            bs.Reset();
            switch( natDetectionAttempts[i].detectionState )
            {
            case STATE_TESTING_NONE_1:
            case STATE_TESTING_NONE_2:
                c = NAT_TYPE_NONE;

#ifdef NTDS_VERBOSE
                printf( "Testing NAT_TYPE_NONE\n" );
#endif
                // S4P5 sends to C2. If arrived, no NAT. Done. (Else S4P5 potentially banned, do not use again).
                saOut = natDetectionAttempts[i].systemAddress;
                saOut.SetPortHostOrder( natDetectionAttempts[i].c2Port );
                bsp.data = (char*)&c;
                bsp.length = 1;
                bsp.systemAddress = saOut;
                s4p5->Send( &bsp, _FILE_AND_LINE_ );
                break;
            case STATE_TESTING_FULL_CONE_1:
            case STATE_TESTING_FULL_CONE_2:

#ifdef NTDS_VERBOSE
                printf( "Testing NAT_TYPE_FULL_CONE\n" );
#endif
                rakPeerInterface->WriteOutOfBandHeader( &bs );
                bs.Write( (unsigned char)ID_NAT_TYPE_DETECT );
                bs.Write( (unsigned char)NAT_TYPE_FULL_CONE );
                // S2P3 sends to C1 (Different address, different port, to previously used port on client). If received, Full-cone nat. Done.  (Else S2P3 potentially banned, do not use again).
                saOut = natDetectionAttempts[i].systemAddress;
                saOut.SetPortHostOrder( natDetectionAttempts[i].systemAddress.GetPort() );
                bsp.data = (char*)bs.GetData();
                bsp.length = bs.GetNumberOfBytesUsed();
                bsp.systemAddress = saOut;
                s2p3->Send( &bsp, _FILE_AND_LINE_ );
                break;
            case STATE_TESTING_ADDRESS_RESTRICTED_1:
            case STATE_TESTING_ADDRESS_RESTRICTED_2:

#ifdef NTDS_VERBOSE
                printf( "Testing NAT_TYPE_ADDRESS_RESTRICTED\n" );
#endif
                rakPeerInterface->WriteOutOfBandHeader( &bs );
                bs.Write( (unsigned char)ID_NAT_TYPE_DETECT );
                bs.Write( (unsigned char)NAT_TYPE_ADDRESS_RESTRICTED );
                // S1P2 sends to C1 (Same address, different port, to previously used port on client). If received, address-restricted cone nat. Done.
                saOut = natDetectionAttempts[i].systemAddress;
                saOut.SetPortHostOrder( natDetectionAttempts[i].systemAddress.GetPort() );
                bsp.data = (char*)bs.GetData();
                bsp.length = bs.GetNumberOfBytesUsed();
                bsp.systemAddress = saOut;
                s1p2->Send( &bsp, _FILE_AND_LINE_ );
                break;
            case STATE_TESTING_PORT_RESTRICTED_1:
            case STATE_TESTING_PORT_RESTRICTED_2:
                // C1 sends to S3P4. If address of C1 as seen by S3P4 is the same as the address of C1 as seen by S1P1, then port-restricted cone nat. Done

#ifdef NTDS_VERBOSE
                printf( "Testing NAT_TYPE_PORT_RESTRICTED\n" );
#endif
                bs.Write( (unsigned char)ID_NAT_TYPE_DETECTION_REQUEST );
                bs.Write( s3p4Address );
                bs.Write( s3p4->GetBoundAddress().GetPort() );
                rakPeerInterface->Send( &bs, HIGH_PRIORITY, RELIABLE, 0, natDetectionAttempts[i].systemAddress, false );
                break;
            default:

#ifdef NTDS_VERBOSE
                printf( "Warning, exceeded final check STATE_TESTING_PORT_RESTRICTED_2.\nExpected that client would have sent NAT_TYPE_PORT_RESTRICTED on s3p4.\nDefaulting to Symmetric\n" );
#endif
                bs.Write( (unsigned char)ID_NAT_TYPE_DETECTION_RESULT );
                bs.Write( (unsigned char)NAT_TYPE_SYMMETRIC );
                rakPeerInterface->Send( &bs, HIGH_PRIORITY, RELIABLE, 0, natDetectionAttempts[i].systemAddress, false );
                natDetectionAttempts.erase( natDetectionAttempts.begin() + i );
                i--;
                break;
            }
        }
        i++;
    }
}

PluginReceiveResult NatTypeDetectionServer::OnReceive( Packet* packet )
{
    switch( packet->data[0] )
    {
    case ID_NAT_TYPE_DETECTION_REQUEST:
        OnDetectionRequest( packet );
        return RR_STOP_PROCESSING_AND_DEALLOCATE;
    }
    return RR_CONTINUE_PROCESSING;
}

void NatTypeDetectionServer::OnClosedConnection( const SystemAddress& systemAddress, RakNetGUID rakNetGUID, PI2_LostConnectionReason lostConnectionReason )
{
    (void)lostConnectionReason;
    (void)rakNetGUID;

    auto it = std::find_if( natDetectionAttempts.begin(), natDetectionAttempts.end(),
                            [&systemAddress]( const NATDetectionAttempt& rAttempt ) { return rAttempt.systemAddress == systemAddress; } );
    if( it == natDetectionAttempts.end() )
        return;
    natDetectionAttempts.erase( it );
}

void NatTypeDetectionServer::OnDetectionRequest( Packet* packet )
{
    auto it = std::find_if( natDetectionAttempts.begin(), natDetectionAttempts.end(),
                            [packet]( const NATDetectionAttempt& rAttempt ) { return rAttempt.systemAddress == packet->systemAddress; } );

    BitStream bsIn( packet->data, packet->length, false );
    bsIn.IgnoreBytes( 1 );
    bool isRequest = false;
    bsIn.Read( isRequest );
    if( isRequest )
    {
        if( it != natDetectionAttempts.end() )
            return; // Already in progress

        NATDetectionAttempt nda;
        nda.detectionState = STATE_NONE;
        nda.systemAddress = packet->systemAddress;
        nda.guid = packet->guid;
        bsIn.Read( nda.c2Port );
        nda.nextStateTime = 0;
        nda.timeBetweenAttempts = rakPeerInterface->GetLastPing( nda.systemAddress ) * 3 + 50;
        natDetectionAttempts.emplace_back( nda );
    }
    else
    {
        if( it == natDetectionAttempts.end() )
            return; // Unknown
        // They are done
        natDetectionAttempts.erase( it );
    }
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void NatTypeDetectionServer::DeallocRNS2RecvStruct( RNS2RecvStruct* s, const char* file, unsigned int line )
{
    RakNet::OP_DELETE( s, file, line );
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
RNS2RecvStruct* NatTypeDetectionServer::AllocRNS2RecvStruct( const char* file, unsigned int line )
{
    return RakNet::OP_NEW<RNS2RecvStruct>( file, line );
}

void NatTypeDetectionServer::OnRNS2Recv( RNS2RecvStruct* recvStruct )
{
    std::lock_guard<std::mutex> guard( bufferedPacketsMutex );
    bufferedPackets.push_back( recvStruct );
}

} // namespace RakNet

#endif // _RAKNET_SUPPORT_*
