/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "OfflineMessagesConvertTest.h"

#include <chrono>
#include <thread>

/*
Description:
Description: Tests / Demonstrates sending messages to systems you are not connected to.

Success conditions:
Proper offline response.
Proper offline ping response.

Failure conditions:
Any success conditions failed

RakPeerInterface Functions used, tested indirectly by its use:
GetGuidFromSystemAddress
Startup
SetMaximumIncomingConnections
Receive
DeallocatePacket

RakPeerInterface Functions Explicitly Tested:
SetOfflinePingResponse
GetOfflinePingResponse
AdvertiseSystem
Ping
*/
int OfflineMessagesConvertTest::RunTest( bool isVerbose, bool noPauses )
{
    bool recievedProperOfflineData = false;
    bool recievedProperPingData = false;

    int nextTest;
    destroyList.clear();

    RakPeerInterface* peer1 = RakPeerInterface::GetInstance();
    destroyList.push_back( peer1 );
    RakPeerInterface* peer2 = RakPeerInterface::GetInstance();
    destroyList.push_back( peer2 );

    bool sentPacket = false;
    nextTest = 0;

    peer1->SetMaximumIncomingConnections( 1 );
    SocketDescriptor socketDescriptor( 60001, 0 );
    peer1->Startup( 1, &socketDescriptor, 1 );
    socketDescriptor.port = 60002;
    peer2->Startup( 1, &socketDescriptor, 1 );
    char* pingResponseData = 0;
    unsigned int responseLen = 0;
    peer1->SetOfflinePingResponse( "Offline Ping Data", (int)strlen( "Offline Ping Data" ) + 1 );
    peer1->GetOfflinePingResponse( &pingResponseData, &responseLen );

    if( strcmp( pingResponseData, "Offline Ping Data" ) != 0 )
    {
        if( isVerbose )
            DebugTools::ShowError( "GetOfflinePingResponse failed.\n", !noPauses && isVerbose, __LINE__, __FILE__ );

        return 5;
    }

    if( isVerbose )
        printf( "Peer 1 guid = %s\n", peer1->GetGuidFromSystemAddress( UNASSIGNED_SYSTEM_ADDRESS ).ToString() );
    if( isVerbose )
        printf( "Peer 2 guid = %s\n", peer2->GetGuidFromSystemAddress( UNASSIGNED_SYSTEM_ADDRESS ).ToString() );
    if( isVerbose )
        printf( "Systems started.  Waiting for advertise system packet\n" );

    // Wait for connection to complete
    std::this_thread::sleep_for( std::chrono::milliseconds( 300 ) );

    if( isVerbose )
        printf( "Sending advertise system from %s\n", peer1->GetGuidFromSystemAddress( UNASSIGNED_SYSTEM_ADDRESS ).ToString() );
    peer1->AdvertiseSystem( "127.0.0.1", 60002, "hello world", (int)strlen( "hello world" ) + 1 );

    TimeMS entryTime = GetTimeMS(); //Loop entry time

    while( nextTest != 2 && GetTimeMS() - entryTime < 10000 ) // run for 10 seconds
    {
        peer1->DeallocatePacket( peer1->Receive() );
        Packet* packet = peer2->Receive();
        if( packet )
        {
            if( packet->data[0] == ID_ADVERTISE_SYSTEM )
            {
                if( packet->length > 1 )
                {
                    if( isVerbose )
                        printf( "Got Advertise system with data: %s\n", packet->data + 1 );

                    if( strcmp( (const char*)( packet->data + 1 ), "hello world" ) == 0 )
                    {
                        recievedProperOfflineData = true;
                    }
                    else
                    {
                        if( isVerbose )
                            DebugTools::ShowError( "Got Advertise system with unexpected data\n", !noPauses && isVerbose, __LINE__, __FILE__ );

                        return 1;
                    }
                }
                else
                {
                    if( isVerbose )
                        DebugTools::ShowError( "Got Advertise system with unexpected data\n", !noPauses && isVerbose, __LINE__, __FILE__ );

                    return 1;
                }
                if( isVerbose )
                    printf( "Was sent from GUID %s\n", packet->guid.ToString() );
                if( isVerbose )
                    printf( "Sending ping from %s\n", peer2->GetGuidFromSystemAddress( UNASSIGNED_SYSTEM_ADDRESS ).ToString() );
                peer2->Ping( "127.0.0.1", 60001, false );
                nextTest++;
            }
            else if( packet->data[0] == ID_UNCONNECTED_PONG )
            {
                // Peer or client. Response from a ping for an unconnected system.
                TimeMS packetTime, dataLength;
                TimeMS curTime = GetTimeMS();
                memcpy( (char*)&packetTime, packet->data + sizeof( unsigned char ), sizeof( TimeMS ) );
                dataLength = packet->length - sizeof( unsigned char ) - sizeof( TimeMS );
                if( peer2->IsLocalIP( packet->systemAddress.ToString( false ) ) )
                {
                    if( isVerbose )
                        printf( "ID_UNCONNECTED_PONG from our own" );
                }
                else
                {
                    if( isVerbose )
                        printf( "ID_UNCONNECTED_PONG from" );
                }
                if( isVerbose )
                {
                    printf( " %s on %p.\nPing is %i\nData is %i bytes long.\n", packet->systemAddress.ToString(), peer2, curTime - packetTime, dataLength );
                    printf( "Was sent from GUID %s\n", packet->guid.ToString() );
                }

                const char* recString = (const char*)( packet->data + sizeof( unsigned char ) + sizeof( TimeMS ) );
                if( dataLength > 0 )
                {
                    printf( "Data is %s\n", recString );

                    if( strcmp( recString, "Offline Ping Data" ) != 0 )
                    {

                        if( isVerbose )
                            DebugTools::ShowError( "Received wrong offline ping response\n", !noPauses && isVerbose, __LINE__, __FILE__ );

                        return 2;
                    }

                    recievedProperPingData = true;
                }
                nextTest++;
                // ProcessUnhandledPacket(packet, ID_UNCONNECTED_PONG,interfaceType);
            }
            peer2->DeallocatePacket( packet );
        }

        std::this_thread::sleep_for( std::chrono::milliseconds( 30 ) );
    }

    if( !recievedProperOfflineData )
    {
        if( isVerbose )
            DebugTools::ShowError( "Never got proper offline data\n", !noPauses && isVerbose, __LINE__, __FILE__ );
        return 3;
    }

    if( !recievedProperPingData )
    {
        if( isVerbose )
            DebugTools::ShowError( "Never got proper ping data\n", !noPauses && isVerbose, __LINE__, __FILE__ );

        return 4;
    }


    return 0;
}

std::string OfflineMessagesConvertTest::GetTestName() const
{
    return "OfflineMessagesConvertTest";
}

std::string OfflineMessagesConvertTest::ErrorCodeToString( int errorCode ) const
{
    // clang-format off
    switch( errorCode )
    {
    case  0: return "No error";                         break;
    case  1: return "Unexpected advertise data";        break;
    case  2: return "Wrong ping response";              break;
    case  3: return "Never got proper offline data";    break;
    case  4: return "Never got proper ping data";       break;
    case  5: return "GetOfflinePingResponse failed.";   break;
    default: return "Undefined Error";                  break;
    }
    // clang-format on
}

OfflineMessagesConvertTest::OfflineMessagesConvertTest( void )
{
}

OfflineMessagesConvertTest::~OfflineMessagesConvertTest( void )
{
}

void OfflineMessagesConvertTest::DestroyPeers()
{
    for( RakPeerInterface* pPeer : destroyList )
    {
        RakPeerInterface::DestroyInstance( pPeer );
    }
}
