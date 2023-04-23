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
#if _RAKNET_SUPPORT_NatPunchthroughClient == 1

#include "Plugins/NatPunchthroughClient.h"
#include "Plugins/PacketLogger.h"
#include "BitStream.h"
#include "MessageIdentifiers.h"
#include "RakPeerInterface.h"
#include "GetTime.h"
#include "StringUtils.h"

#include <algorithm>

namespace RakNet {

void NatPunchthroughDebugInterface_Printf::OnClientMessage( const char* msg )
{
    printf( "%s\n", msg );
}
#if _RAKNET_SUPPORT_PacketLogger == 1
void NatPunchthroughDebugInterface_PacketLogger::OnClientMessage( const char* msg )
{
    if( pl )
    {
        pl->WriteMiscellaneous( "Nat", msg );
    }
}
#endif

STATIC_FACTORY_DEFINITIONS( NatPunchthroughClient, NatPunchthroughClient );

NatPunchthroughClient::NatPunchthroughClient()
{
    natPunchthroughDebugInterface = 0;
    mostRecentExternalPort = 0;
    sp.nextActionTime = 0;
    portStride = 0;
    hasPortStride = UNKNOWN_PORT_STRIDE;
}
NatPunchthroughClient::~NatPunchthroughClient()
{
    rakPeerInterface = 0;
    Clear();
}
void NatPunchthroughClient::FindRouterPortStride( const SystemAddress& facilitator )
{
    ConnectionState cs = rakPeerInterface->GetConnectionState( facilitator );
    if( cs != IS_CONNECTED )
        return;
    if( hasPortStride != UNKNOWN_PORT_STRIDE )
        return;

    hasPortStride = CALCULATING_PORT_STRIDE;
    portStrideCalTimeout = RakNet::GetTime() + 5000;

    if( natPunchthroughDebugInterface )
    {
        natPunchthroughDebugInterface->OnClientMessage( RakNet::format( "Calculating port stride from %s", facilitator.ToString( true ) ).c_str() );
    }

    BitStream outgoingBs;
    outgoingBs.Write( (MessageID)ID_NAT_REQUEST_BOUND_ADDRESSES );
    rakPeerInterface->Send( &outgoingBs, HIGH_PRIORITY, RELIABLE_ORDERED, 0, facilitator, false );
}
bool NatPunchthroughClient::OpenNAT( RakNetGUID destination, const SystemAddress& facilitator )
{
    ConnectionState cs = rakPeerInterface->GetConnectionState( facilitator );
    if( cs != IS_CONNECTED )
        return false;
    if( hasPortStride == UNKNOWN_PORT_STRIDE )
    {
        FindRouterPortStride( facilitator );
        QueueOpenNAT( destination, facilitator );
    }
    else if( hasPortStride == CALCULATING_PORT_STRIDE )
    {
        QueueOpenNAT( destination, facilitator );
    }
    else
    {
        SendPunchthrough( destination, facilitator );
    }

    return true;
}

void NatPunchthroughClient::SetDebugInterface( NatPunchthroughDebugInterface* i )
{
    natPunchthroughDebugInterface = i;
}
void NatPunchthroughClient::Update( void )
{
    RakNet::Time time = RakNet::GetTime();

    if( hasPortStride == CALCULATING_PORT_STRIDE && time > portStrideCalTimeout )
    {
        if( natPunchthroughDebugInterface )
        {
            natPunchthroughDebugInterface->OnClientMessage( "CALCULATING_PORT_STRIDE timeout" );
        }

        SendQueuedOpenNAT();
        hasPortStride = UNKNOWN_PORT_STRIDE;
    }

    if( sp.nextActionTime && sp.nextActionTime < time )
    {
        RakNet::Time delta = time - sp.nextActionTime;
        if( sp.testMode == SendPing::TESTING_INTERNAL_IPS )
        {
            SendOutOfBand( sp.internalIds[sp.attemptCount], ID_NAT_ESTABLISH_UNIDIRECTIONAL );

            if( ++sp.retryCount >= pc.UDP_SENDS_PER_PORT_INTERNAL )
            {
                ++sp.attemptCount;
                sp.retryCount = 0;
            }

            if( sp.attemptCount >= pc.MAXIMUM_NUMBER_OF_INTERNAL_IDS_TO_CHECK )
            {
                sp.testMode = SendPing::WAITING_FOR_INTERNAL_IPS_RESPONSE;
                if( pc.INTERNAL_IP_WAIT_AFTER_ATTEMPTS > 0 )
                {
                    sp.nextActionTime = time + pc.INTERNAL_IP_WAIT_AFTER_ATTEMPTS - delta;
                }
                else
                {
                    sp.testMode = SendPing::TESTING_EXTERNAL_IPS_FACILITATOR_PORT_TO_FACILITATOR_PORT;
                    sp.attemptCount = 0;
                    sp.sentTTL = false;
                }
            }
            else
            {
                sp.nextActionTime = time + pc.TIME_BETWEEN_PUNCH_ATTEMPTS_INTERNAL - delta;
            }
        }
        else if( sp.testMode == SendPing::WAITING_FOR_INTERNAL_IPS_RESPONSE )
        {
            sp.testMode = SendPing::TESTING_EXTERNAL_IPS_FACILITATOR_PORT_TO_FACILITATOR_PORT;
            sp.attemptCount = 0;
            sp.sentTTL = false;
        }
        else if( sp.testMode == SendPing::TESTING_EXTERNAL_IPS_FACILITATOR_PORT_TO_FACILITATOR_PORT )
        {
            if( sp.sentTTL == false )
            {
                SystemAddress sa = sp.targetAddress;
                sa.SetPortHostOrder( (unsigned short)( sa.GetPort() + sp.attemptCount ) );
                SendTTL( sa );

                if( natPunchthroughDebugInterface )
                {
                    natPunchthroughDebugInterface->OnClientMessage( RakNet::format( "Send with TTL 2 to %s", sa.ToString( true ) ).c_str() );
                }

                sp.nextActionTime = time + pc.EXTERNAL_IP_WAIT_AFTER_FIRST_TTL - delta;
                sp.sentTTL = true;
            }
            else
            {
                SystemAddress sa = sp.targetAddress;
                sa.SetPortHostOrder( (unsigned short)( sa.GetPort() + sp.attemptCount ) );
                SendOutOfBand( sa, ID_NAT_ESTABLISH_UNIDIRECTIONAL );

                IncrementExternalAttemptCount( time, delta );

                if( sp.attemptCount > pc.MAX_PREDICTIVE_PORT_RANGE )
                {
                    sp.testMode = SendPing::WAITING_AFTER_ALL_ATTEMPTS;
                    sp.nextActionTime = time + pc.EXTERNAL_IP_WAIT_AFTER_ALL_ATTEMPTS - delta;

                    // Skip TESTING_EXTERNAL_IPS_1024_TO_FACILITATOR_PORT, etc.
                    /*
                    sp.testMode=SendPing::TESTING_EXTERNAL_IPS_1024_TO_FACILITATOR_PORT;
                    sp.attemptCount=0;
                    */
                }
            }
        }
        else if( sp.testMode == SendPing::TESTING_EXTERNAL_IPS_1024_TO_FACILITATOR_PORT )
        {
            SystemAddress sa = sp.targetAddress;
            if( sp.targetGuid < rakPeerInterface->GetGuidFromSystemAddress( UNASSIGNED_SYSTEM_ADDRESS ) )
                sa.SetPortHostOrder( (unsigned short)( 1024 + sp.attemptCount ) );
            else
                sa.SetPortHostOrder( (unsigned short)( sa.GetPort() + sp.attemptCount ) );
            SendOutOfBand( sa, ID_NAT_ESTABLISH_UNIDIRECTIONAL );

            IncrementExternalAttemptCount( time, delta );

            if( sp.attemptCount > pc.MAX_PREDICTIVE_PORT_RANGE )
            {
                // From 1024 disabled, never helps as I've seen, but slows down the process by half
                sp.testMode = SendPing::TESTING_EXTERNAL_IPS_FACILITATOR_PORT_TO_1024;
                sp.attemptCount = 0;
            }
        }
        else if( sp.testMode == SendPing::TESTING_EXTERNAL_IPS_FACILITATOR_PORT_TO_1024 )
        {
            SystemAddress sa = sp.targetAddress;
            if( sp.targetGuid > rakPeerInterface->GetGuidFromSystemAddress( UNASSIGNED_SYSTEM_ADDRESS ) )
                sa.SetPortHostOrder( (unsigned short)( 1024 + sp.attemptCount ) );
            else
                sa.SetPortHostOrder( (unsigned short)( sa.GetPort() + sp.attemptCount ) );
            SendOutOfBand( sa, ID_NAT_ESTABLISH_UNIDIRECTIONAL );

            IncrementExternalAttemptCount( time, delta );

            if( sp.attemptCount > pc.MAX_PREDICTIVE_PORT_RANGE )
            {
                // From 1024 disabled, never helps as I've seen, but slows down the process by half
                sp.testMode = SendPing::TESTING_EXTERNAL_IPS_1024_TO_1024;
                sp.attemptCount = 0;
            }
        }
        else if( sp.testMode == SendPing::TESTING_EXTERNAL_IPS_1024_TO_1024 )
        {
            SystemAddress sa = sp.targetAddress;
            sa.SetPortHostOrder( (unsigned short)( 1024 + sp.attemptCount ) );
            SendOutOfBand( sa, ID_NAT_ESTABLISH_UNIDIRECTIONAL );

            IncrementExternalAttemptCount( time, delta );

            if( sp.attemptCount > pc.MAX_PREDICTIVE_PORT_RANGE )
            {
                if( natPunchthroughDebugInterface )
                {
                    char ipAddressString[32];
                    sp.targetAddress.ToString( true, ipAddressString );
                    char guidString[128];
                    sp.targetGuid.ToString( guidString );
                    natPunchthroughDebugInterface->OnClientMessage( RakNet::format( "Likely bidirectional punchthrough failure to guid %s, system address %s.", guidString, ipAddressString ).c_str() );
                }

                sp.testMode = SendPing::WAITING_AFTER_ALL_ATTEMPTS;
                sp.nextActionTime = time + pc.EXTERNAL_IP_WAIT_AFTER_ALL_ATTEMPTS - delta;
            }
        }
        else if( sp.testMode == SendPing::WAITING_AFTER_ALL_ATTEMPTS )
        {
            // Failed. Tell the user
            OnPunchthroughFailure();
        }

        if( sp.testMode == SendPing::PUNCHING_FIXED_PORT )
        {
            SendOutOfBand( sp.targetAddress, ID_NAT_ESTABLISH_BIDIRECTIONAL );
            if( ++sp.retryCount >= sp.punchingFixedPortAttempts )
            {
                if( natPunchthroughDebugInterface )
                {
                    char ipAddressString[32];
                    sp.targetAddress.ToString( true, ipAddressString );
                    char guidString[128];
                    sp.targetGuid.ToString( guidString );
                    natPunchthroughDebugInterface->OnClientMessage( RakNet::format( "Likely unidirectional punchthrough failure to guid %s, system address %s.", guidString, ipAddressString ).c_str() );
                }

                sp.testMode = SendPing::WAITING_AFTER_ALL_ATTEMPTS;
                sp.nextActionTime = time + pc.EXTERNAL_IP_WAIT_AFTER_ALL_ATTEMPTS - delta;
            }
            else
            {
                if( ( sp.retryCount % pc.UDP_SENDS_PER_PORT_EXTERNAL ) == 0 )
                    sp.nextActionTime = time + pc.EXTERNAL_IP_WAIT_BETWEEN_PORTS - delta;
                else
                    sp.nextActionTime = time + pc.TIME_BETWEEN_PUNCH_ATTEMPTS_EXTERNAL - delta;
            }
        }
    }
}
void NatPunchthroughClient::PushFailure( void )
{
    Packet* p = AllocatePacketUnified( sizeof( MessageID ) + sizeof( unsigned char ) );
    p->data[0] = ID_NAT_PUNCHTHROUGH_FAILED;
    p->systemAddress = sp.targetAddress;
    p->systemAddress.systemIndex = (SystemIndex)-1;
    p->guid = sp.targetGuid;
    if( sp.weAreSender )
        p->data[1] = 1;
    else
        p->data[1] = 0;
    p->wasGeneratedLocally = true;
    rakPeerInterface->PushBackPacket( p, true );
}
void NatPunchthroughClient::OnPunchthroughFailure( void )
{
    if( pc.retryOnFailure == false )
    {
        if( natPunchthroughDebugInterface )
        {
            char ipAddressString[32];
            sp.targetAddress.ToString( true, ipAddressString );
            char guidString[128];
            sp.targetGuid.ToString( guidString );
            natPunchthroughDebugInterface->OnClientMessage( RakNet::format( "Failed punchthrough once. Returning failure to guid %s, system address %s to user.", guidString, ipAddressString ).c_str() );
        }

        PushFailure();
        OnReadyForNextPunchthrough();
        return;
    }

    auto it = std::find_if( failedAttemptList.begin(), failedAttemptList.end(),
                            [&]( const AddrAndGuid& rAddr ) { return rAddr.guid == sp.targetGuid; } );
    if( it != failedAttemptList.end() )
    {
        if( natPunchthroughDebugInterface )
        {
            char ipAddressString[32];
            sp.targetAddress.ToString( true, ipAddressString );
            char guidString[128];
            sp.targetGuid.ToString( guidString );
            natPunchthroughDebugInterface->OnClientMessage( RakNet::format( "Failed punchthrough twice. Returning failure to guid %s, system address %s to user.", guidString, ipAddressString ).c_str() );
        }

        // Failed a second time, so return failed to user
        PushFailure();

        OnReadyForNextPunchthrough();

        failedAttemptList.erase( it );
        return;
    }

    if( rakPeerInterface->GetConnectionState( sp.facilitator ) != IS_CONNECTED )
    {
        if( natPunchthroughDebugInterface )
        {
            char ipAddressString[32];
            sp.targetAddress.ToString( true, ipAddressString );
            char guidString[128];
            sp.targetGuid.ToString( guidString );
            natPunchthroughDebugInterface->OnClientMessage( RakNet::format( "Not connected to facilitator, so cannot retry punchthrough after first failure. Returning failure onj guid %s, system address %s to user.", guidString, ipAddressString ).c_str() );
        }

        // Failed, and can't try again because no facilitator
        PushFailure();
        return;
    }

    if( natPunchthroughDebugInterface )
    {
        char ipAddressString[32];
        sp.targetAddress.ToString( true, ipAddressString );
        char guidString[128];
        sp.targetGuid.ToString( guidString );
        natPunchthroughDebugInterface->OnClientMessage( RakNet::format( "First punchthrough failure on guid %s, system address %s. Reattempting.", guidString, ipAddressString ).c_str() );
    }

    // Failed the first time. Add to the failure queue and try again
    failedAttemptList.emplace_back( AddrAndGuid{ sp.targetAddress, sp.targetGuid } );

    // Tell the server we are ready
    OnReadyForNextPunchthrough();

    // If we are the sender, try again, immediately if possible, else added to the queue on the faciltiator
    if( sp.weAreSender )
        SendPunchthrough( sp.targetGuid, sp.facilitator );
}
PluginReceiveResult NatPunchthroughClient::OnReceive( Packet* packet )
{
    switch( packet->data[0] )
    {
    case ID_NAT_GET_MOST_RECENT_PORT: {
        OnGetMostRecentPort( packet );
        return RR_STOP_PROCESSING_AND_DEALLOCATE;
    }
    case ID_NAT_PUNCHTHROUGH_FAILED:
    case ID_NAT_PUNCHTHROUGH_SUCCEEDED:
        if( packet->wasGeneratedLocally == false )
            return RR_STOP_PROCESSING_AND_DEALLOCATE;
        break;
    case ID_NAT_RESPOND_BOUND_ADDRESSES: {
        BitStream bs( packet->data, packet->length, false );
        bs.IgnoreBytes( sizeof( MessageID ) );
        unsigned char boundAddressCount;
        bs.Read( boundAddressCount );
        if( boundAddressCount < 2 )
        {
            if( natPunchthroughDebugInterface )
                natPunchthroughDebugInterface->OnClientMessage( RakNet::format( "INCAPABLE_PORT_STRIDE. My external ID is %s", rakPeerInterface->GetExternalID( packet->systemAddress ).ToString() ).c_str() );

            hasPortStride = INCAPABLE_PORT_STRIDE;
            SendQueuedOpenNAT();
        }
        SystemAddress boundAddresses[MAXIMUM_NUMBER_OF_INTERNAL_IDS];
        for( int i = 0; i < boundAddressCount && i < MAXIMUM_NUMBER_OF_INTERNAL_IDS; i++ )
        {
            bs.Read( boundAddresses[i] );
            if( boundAddresses[i] != packet->systemAddress )
            {
                BitStream outgoingBs;
                outgoingBs.Write( (MessageID)ID_NAT_PING );
                uint16_t externalPort = rakPeerInterface->GetExternalID( packet->systemAddress ).GetPort();
                outgoingBs.Write( externalPort );
                rakPeerInterface->SendOutOfBand( (const char*)boundAddresses[i].ToString( false ), boundAddresses[i].GetPort(), (const char*)outgoingBs.GetData(), outgoingBs.GetNumberOfBytesUsed() );
                break;
            }
        }
    }
        return RR_STOP_PROCESSING_AND_DEALLOCATE;
    case ID_OUT_OF_BAND_INTERNAL:
        if( packet->length >= 2 && packet->data[1] == ID_NAT_PONG )
        {
            BitStream bs( packet->data, packet->length, false );
            bs.IgnoreBytes( sizeof( MessageID ) * 2 );
            uint16_t externalPort;
            bs.Read( externalPort );
            uint16_t externalPort2;
            bs.Read( externalPort2 );
            portStride = externalPort2 - externalPort;
            mostRecentExternalPort = externalPort2;
            hasPortStride = HAS_PORT_STRIDE;

            if( natPunchthroughDebugInterface )
                natPunchthroughDebugInterface->OnClientMessage( RakNet::format( "HAS_PORT_STRIDE %i. First external port %i. Second external port %i.", portStride, externalPort, externalPort2 ).c_str() );

            SendQueuedOpenNAT();
            return RR_STOP_PROCESSING_AND_DEALLOCATE;
        }
        else if( packet->length >= 2 &&
                 ( packet->data[1] == ID_NAT_ESTABLISH_UNIDIRECTIONAL || packet->data[1] == ID_NAT_ESTABLISH_BIDIRECTIONAL ) &&
                 sp.nextActionTime != 0 )
        {
            BitStream bs( packet->data, packet->length, false );
            bs.IgnoreBytes( 2 );
            uint16_t sessionId;
            bs.Read( sessionId );
            //          RakAssert(sessionId<100);
            if( sessionId != sp.sessionId )
                break;

            char ipAddressString[32];
            packet->systemAddress.ToString( true, ipAddressString );
            // sp.targetGuid==packet->guid is because the internal IP addresses reported may include loopbacks not reported by RakPeer::IsLocalIP()
            if( packet->data[1] == ID_NAT_ESTABLISH_UNIDIRECTIONAL && sp.targetGuid == packet->guid )
            {

                if( sp.testMode != SendPing::PUNCHING_FIXED_PORT )
                {
                    sp.testMode = SendPing::PUNCHING_FIXED_PORT;
                    sp.retryCount += sp.attemptCount * pc.UDP_SENDS_PER_PORT_EXTERNAL;
                    sp.targetAddress = packet->systemAddress;
                    // Keeps trying until the other side gives up too, in case it is unidirectional
                    sp.punchingFixedPortAttempts = pc.UDP_SENDS_PER_PORT_EXTERNAL * ( pc.MAX_PREDICTIVE_PORT_RANGE + 1 );

                    if( natPunchthroughDebugInterface )
                    {
                        char guidString[128];
                        sp.targetGuid.ToString( guidString );
                        natPunchthroughDebugInterface->OnClientMessage( RakNet::format( "PUNCHING_FIXED_PORT: Received ID_NAT_ESTABLISH_UNIDIRECTIONAL from guid %s, system address %s.", guidString, ipAddressString ).c_str() );
                    }
                }
                else
                {
                    if( natPunchthroughDebugInterface )
                    {
                        char guidString[128];
                        sp.targetGuid.ToString( guidString );
                        natPunchthroughDebugInterface->OnClientMessage( RakNet::format( "Received ID_NAT_ESTABLISH_UNIDIRECTIONAL from guid %s, system address %s.", guidString, ipAddressString ).c_str() );
                    }
                }

                SendOutOfBand( sp.targetAddress, ID_NAT_ESTABLISH_BIDIRECTIONAL );
            }
            else if( packet->data[1] == ID_NAT_ESTABLISH_BIDIRECTIONAL &&
                     sp.targetGuid == packet->guid )
            {
                // They send back our port
                unsigned short ourExternalPort;
                bs.Read( ourExternalPort );
                if( mostRecentExternalPort == 0 )
                {
                    mostRecentExternalPort = ourExternalPort;

                    if( natPunchthroughDebugInterface )
                    {
                        natPunchthroughDebugInterface->OnClientMessage( RakNet::format( "ID_NAT_ESTABLISH_BIDIRECTIONAL mostRecentExternalPort first time set to %i", mostRecentExternalPort ).c_str() );
                    }
                }
                else
                {
                    if( sp.testMode != SendPing::TESTING_INTERNAL_IPS && sp.testMode != SendPing::WAITING_FOR_INTERNAL_IPS_RESPONSE )
                    {
                        if( hasPortStride != HAS_PORT_STRIDE )
                        {
                            portStride = ourExternalPort - mostRecentExternalPort;
                            hasPortStride = HAS_PORT_STRIDE;

                            if( natPunchthroughDebugInterface )
                            {
                                natPunchthroughDebugInterface->OnClientMessage( RakNet::format( "ID_NAT_ESTABLISH_BIDIRECTIONAL: Estimated port stride from incoming connection at %i. ourExternalPort=%i mostRecentExternalPort=%i", portStride, ourExternalPort, mostRecentExternalPort ).c_str() );
                            }

                            SendQueuedOpenNAT();
                        }

                        //nextExternalPort += portStride * (pc.MAX_PREDICTIVE_PORT_RANGE+1);
                        mostRecentExternalPort = ourExternalPort;

                        if( natPunchthroughDebugInterface )
                        {
                            natPunchthroughDebugInterface->OnClientMessage( RakNet::format( "ID_NAT_ESTABLISH_BIDIRECTIONAL: New mostRecentExternalPort %i", mostRecentExternalPort ).c_str() );
                        }
                    }
                }
                SendOutOfBand( packet->systemAddress, ID_NAT_ESTABLISH_BIDIRECTIONAL );

                // Tell the user about the success
                sp.targetAddress = packet->systemAddress;
                PushSuccess();
                OnReadyForNextPunchthrough();
                bool removedFromFailureQueue = RemoveFromFailureQueue();

                if( natPunchthroughDebugInterface )
                {
                    char guidString[128];
                    sp.targetGuid.ToString( guidString );
                    if( removedFromFailureQueue )
                        natPunchthroughDebugInterface->OnClientMessage( RakNet::format( "Punchthrough to guid %s, system address %s succeeded on 2nd attempt.", guidString, ipAddressString ).c_str() );
                    else
                        natPunchthroughDebugInterface->OnClientMessage( RakNet::format( "Punchthrough to guid %s, system address %s succeeded on 1st attempt.", guidString, ipAddressString ).c_str() );
                }
            }

            //      mostRecentNewExternalPort=packet->systemAddress.GetPort();
        }
        return RR_STOP_PROCESSING_AND_DEALLOCATE;
    case ID_NAT_ALREADY_IN_PROGRESS: {
        BitStream incomingBs( packet->data, packet->length, false );
        incomingBs.IgnoreBytes( sizeof( MessageID ) );
        RakNetGUID targetGuid;
        incomingBs.Read( targetGuid );

        if( natPunchthroughDebugInterface )
        {
            char guidString[128];
            targetGuid.ToString( guidString );
            natPunchthroughDebugInterface->OnClientMessage( RakNet::format( "Punchthrough retry to guid %s failed due to ID_NAT_ALREADY_IN_PROGRESS. Returning failure.", guidString ).c_str() );
        }
    }
    break;
    case ID_NAT_TARGET_NOT_CONNECTED:
    case ID_NAT_CONNECTION_TO_TARGET_LOST:
    case ID_NAT_TARGET_UNRESPONSIVE: {
        const char* reason;
        if( packet->data[0] == ID_NAT_TARGET_NOT_CONNECTED )
            reason = (char*)"ID_NAT_TARGET_NOT_CONNECTED";
        else if( packet->data[0] == ID_NAT_CONNECTION_TO_TARGET_LOST )
            reason = (char*)"ID_NAT_CONNECTION_TO_TARGET_LOST";
        else
            reason = (char*)"ID_NAT_TARGET_UNRESPONSIVE";


        BitStream incomingBs( packet->data, packet->length, false );
        incomingBs.IgnoreBytes( sizeof( MessageID ) );

        RakNetGUID targetGuid;
        incomingBs.Read( targetGuid );

        if( packet->data[0] == ID_NAT_CONNECTION_TO_TARGET_LOST ||
            packet->data[0] == ID_NAT_TARGET_UNRESPONSIVE )
        {
            uint16_t sessionId;
            incomingBs.Read( sessionId );
            if( sessionId != sp.sessionId )
                break;
        }

        auto it = std::find_if( failedAttemptList.begin(), failedAttemptList.end(),
                                [&targetGuid]( const AddrAndGuid& rAddr ) { return rAddr.guid == targetGuid; } );
        if( it != failedAttemptList.end() )
        {
            if( natPunchthroughDebugInterface )
            {
                char guidString[128];
                targetGuid.ToString( guidString );
                natPunchthroughDebugInterface->OnClientMessage( RakNet::format( "Punchthrough retry to guid %s failed due to %s.", guidString, reason ).c_str() );
            }

            // If the retry target is not connected, or loses connection, or is not responsive, then previous failures cannot be retried.

            // Don't need to return failed, the other messages indicate failure anyway
            /*
            Packet *p = AllocatePacketUnified(sizeof(MessageID));
            p->data[0]=ID_NAT_PUNCHTHROUGH_FAILED;
            p->systemAddress=failedAttemptList[i].addr;
            p->systemAddress.systemIndex=(SystemIndex)-1;
            p->guid=failedAttemptList[i].guid;
            rakPeerInterface->PushBackPacket(p, false);
            */

            failedAttemptList.erase( it );
            break;
        }

        if( natPunchthroughDebugInterface )
        {
            char guidString[128];
            targetGuid.ToString( guidString );
            natPunchthroughDebugInterface->OnClientMessage( RakNet::format( "Punchthrough attempt to guid %s failed due to %s.", guidString, reason ).c_str() );
        }

        // Stop trying punchthrough
        sp.nextActionTime = 0;
    }
    break;
    case ID_TIMESTAMP:
        if( packet->data[sizeof( MessageID ) + sizeof( RakNet::Time )] == ID_NAT_CONNECT_AT_TIME )
        {
            OnConnectAtTime( packet );
            return RR_STOP_PROCESSING_AND_DEALLOCATE;
        }
        break;
    }
    return RR_CONTINUE_PROCESSING;
}
void NatPunchthroughClient::OnConnectAtTime( Packet* packet )
{
    //  RakAssert(sp.nextActionTime==0);

    BitStream bs( packet->data, packet->length, false );
    bs.IgnoreBytes( sizeof( MessageID ) );
    bs.Read( sp.nextActionTime );
    bs.IgnoreBytes( sizeof( MessageID ) );
    bs.Read( sp.sessionId );
    bs.Read( sp.targetAddress );
    int j;
    //  int k;
    //  k=0;
    for( j = 0; j < MAXIMUM_NUMBER_OF_INTERNAL_IDS; j++ )
        bs.Read( sp.internalIds[j] );

    // Prevents local testing
    /*
    for (j=0; j < MAXIMUM_NUMBER_OF_INTERNAL_IDS; j++)
    {
        SystemAddress id;
        bs.Read(id);
        char str[32];
        id.ToString(false,str);
        if (rakPeerInterface->IsLocalIP(str)==false)
            sp.internalIds[k++]=id;
    }
    */
    sp.attemptCount = 0;
    sp.retryCount = 0;
    if( pc.MAXIMUM_NUMBER_OF_INTERNAL_IDS_TO_CHECK > 0 )
    {
        sp.testMode = SendPing::TESTING_INTERNAL_IPS;
    }
    else
    {
        // TESTING: Try sending to unused ports on the remote system to reserve our own ports while not getting banned
        sp.testMode = SendPing::TESTING_EXTERNAL_IPS_FACILITATOR_PORT_TO_FACILITATOR_PORT;
        sp.attemptCount = 0;
        sp.sentTTL = false;
    }
    bs.Read( sp.targetGuid );
    bs.Read( sp.weAreSender );
}
void NatPunchthroughClient::SendTTL( const SystemAddress& sa )
{
    if( sa == UNASSIGNED_SYSTEM_ADDRESS )
        return;
    if( sa.GetPort() == 0 )
        return;

    char ipAddressString[32];
    sa.ToString( false, ipAddressString );
    // TTL of 1 doesn't get past the router, 2 might hit the other system on a LAN
    rakPeerInterface->SendTTL( ipAddressString, sa.GetPort(), 2 );
}

const char* TestModeToString( NatPunchthroughClient::SendPing::TestMode tm )
{
    switch( tm )
    {
    case NatPunchthroughClient::SendPing::TESTING_INTERNAL_IPS:
        return "TESTING_INTERNAL_IPS";
        break;
    case NatPunchthroughClient::SendPing::WAITING_FOR_INTERNAL_IPS_RESPONSE:
        return "WAITING_FOR_INTERNAL_IPS_RESPONSE";
        break;
    case NatPunchthroughClient::SendPing::TESTING_EXTERNAL_IPS_FACILITATOR_PORT_TO_FACILITATOR_PORT:
        return "TESTING_EXTERNAL_IPS_FACILITATOR_PORT_TO_FACILITATOR_PORT";
        break;
    case NatPunchthroughClient::SendPing::TESTING_EXTERNAL_IPS_1024_TO_FACILITATOR_PORT:
        return "TESTING_EXTERNAL_IPS_1024_TO_FACILITATOR_PORT";
        break;
    case NatPunchthroughClient::SendPing::TESTING_EXTERNAL_IPS_FACILITATOR_PORT_TO_1024:
        return "TESTING_EXTERNAL_IPS_FACILITATOR_PORT_TO_1024";
        break;
    case NatPunchthroughClient::SendPing::TESTING_EXTERNAL_IPS_1024_TO_1024:
        return "TESTING_EXTERNAL_IPS_1024_TO_1024";
        break;
    case NatPunchthroughClient::SendPing::WAITING_AFTER_ALL_ATTEMPTS:
        return "WAITING_AFTER_ALL_ATTEMPTS";
        break;
    case NatPunchthroughClient::SendPing::PUNCHING_FIXED_PORT:
        return "PUNCHING_FIXED_PORT";
        break;
    }
    return "";
}
void NatPunchthroughClient::SendOutOfBand( SystemAddress sa, MessageID oobId )
{
    if( sa == UNASSIGNED_SYSTEM_ADDRESS )
        return;
    if( sa.GetPort() == 0 )
        return;

    BitStream oob;
    oob.Write( oobId );
    oob.Write( sp.sessionId );
    //  RakAssert(sp.sessionId<100);
    if( oobId == ID_NAT_ESTABLISH_BIDIRECTIONAL )
        oob.Write( sa.GetPort() );
    char ipAddressString[32];
    sa.ToString( false, ipAddressString );
    rakPeerInterface->SendOutOfBand( (const char*)ipAddressString, sa.GetPort(), (const char*)oob.GetData(), oob.GetNumberOfBytesUsed() );

    if( natPunchthroughDebugInterface )
    {
        sa.ToString( true, ipAddressString );
        char guidString[128];
        sp.targetGuid.ToString( guidString );

        // server - diff = my time
        // server = myTime + diff
        RakNet::Time clockDifferential = rakPeerInterface->GetClockDifferential( sp.facilitator );
        RakNet::Time serverTime = RakNet::GetTime() + clockDifferential;

        if( oobId == ID_NAT_ESTABLISH_UNIDIRECTIONAL )
#if defined( _WIN32 )
            natPunchthroughDebugInterface->OnClientMessage( RakNet::format( "%I64d: %s: OOB ID_NAT_ESTABLISH_UNIDIRECTIONAL to guid %s, system address %s.\n", serverTime, TestModeToString( sp.testMode ), guidString, ipAddressString ).c_str() );
#else
            natPunchthroughDebugInterface->OnClientMessage( RakNet::format( "%lld: %s: OOB ID_NAT_ESTABLISH_UNIDIRECTIONAL to guid %s, system address %s.\n", serverTime, TestModeToString( sp.testMode ), guidString, ipAddressString ).c_str() );
#endif
        else
#if defined( _WIN32 )
            natPunchthroughDebugInterface->OnClientMessage( RakNet::format( "%I64d: %s: OOB ID_NAT_ESTABLISH_BIDIRECTIONAL to guid %s, system address %s.\n", serverTime, TestModeToString( sp.testMode ), guidString, ipAddressString ).c_str() );
#else
            natPunchthroughDebugInterface->OnClientMessage( RakNet::format( "%lld: %s: OOB ID_NAT_ESTABLISH_BIDIRECTIONAL to guid %s, system address %s.\n", serverTime, TestModeToString( sp.testMode ), guidString, ipAddressString ).c_str() );
#endif
    }
}
void NatPunchthroughClient::OnNewConnection( const SystemAddress& systemAddress, RakNetGUID rakNetGUID, bool isIncoming )
{
    (void)rakNetGUID;
    (void)isIncoming;

    // Try to track new port mappings on the router. Not reliable, but better than nothing.
    SystemAddress ourExternalId = rakPeerInterface->GetExternalID( systemAddress );
    if( ourExternalId != UNASSIGNED_SYSTEM_ADDRESS && mostRecentExternalPort == 0 )
    {
        mostRecentExternalPort = ourExternalId.GetPort();

        if( natPunchthroughDebugInterface )
        {
            natPunchthroughDebugInterface->OnClientMessage( RakNet::format( "OnNewConnection mostRecentExternalPort first time set to %i", mostRecentExternalPort ).c_str() );
        }
    }
}

void NatPunchthroughClient::OnClosedConnection( const SystemAddress& systemAddress, RakNetGUID rakNetGUID, PI2_LostConnectionReason lostConnectionReason )
{
    (void)rakNetGUID;
    (void)lostConnectionReason;

    if( sp.facilitator == systemAddress )
    {
        // If we lose the connection to the facilitator, all previous failures not currently in progress are returned as such
        for( auto it = failedAttemptList.begin(); it != failedAttemptList.end(); /**/ )
        {
            if( sp.nextActionTime != 0 && sp.targetGuid == it->guid )
            {
                ++it;
                continue;
            }

            PushFailure();

            it = failedAttemptList.erase( it );
        }
    }
}
void NatPunchthroughClient::OnFailureNotification( Packet* packet )
{
    BitStream incomingBs( packet->data, packet->length, false );
    incomingBs.IgnoreBytes( sizeof( MessageID ) );
    RakNetGUID senderGuid;
    incomingBs.Read( senderGuid );
}
void NatPunchthroughClient::OnGetMostRecentPort( Packet* packet )
{
    BitStream incomingBs( packet->data, packet->length, false );
    incomingBs.IgnoreBytes( sizeof( MessageID ) );
    uint16_t sessionId;
    incomingBs.Read( sessionId );

    BitStream outgoingBs;
    outgoingBs.Write( (MessageID)ID_NAT_GET_MOST_RECENT_PORT );
    outgoingBs.Write( sessionId );
    if( mostRecentExternalPort == 0 )
    {
        mostRecentExternalPort = rakPeerInterface->GetExternalID( packet->systemAddress ).GetPort();
        RakAssert( mostRecentExternalPort != 0 );

        if( natPunchthroughDebugInterface )
        {
            natPunchthroughDebugInterface->OnClientMessage( RakNet::format( "OnGetMostRecentPort mostRecentExternalPort first time set to %i", mostRecentExternalPort ).c_str() );
        }
    }

    unsigned short portWithStride;
    if( hasPortStride == HAS_PORT_STRIDE )
        portWithStride = mostRecentExternalPort + portStride;
    else
        portWithStride = mostRecentExternalPort;
    outgoingBs.Write( portWithStride );

    rakPeerInterface->Send( &outgoingBs, HIGH_PRIORITY, RELIABLE_ORDERED, 0, packet->systemAddress, false );
    sp.facilitator = packet->systemAddress;
}

void NatPunchthroughClient::QueueOpenNAT( RakNetGUID destination, const SystemAddress& facilitator )
{
    DSTAndFac daf;
    daf.destination = destination;
    daf.facilitator = facilitator;
    queuedOpenNat.push_back( daf );
}
void NatPunchthroughClient::SendQueuedOpenNAT( void )
{
    while( !queuedOpenNat.empty() )
    {
        DSTAndFac daf = queuedOpenNat.front();
        queuedOpenNat.pop_back(),
        SendPunchthrough( daf.destination, daf.facilitator );
    }
}
void NatPunchthroughClient::SendPunchthrough( RakNetGUID destination, const SystemAddress& facilitator )
{
    BitStream outgoingBs;
    outgoingBs.Write( (MessageID)ID_NAT_PUNCHTHROUGH_REQUEST );
    outgoingBs.Write( destination );
    rakPeerInterface->Send( &outgoingBs, HIGH_PRIORITY, RELIABLE_ORDERED, 0, facilitator, false );

    //  RakAssert(rakPeerInterface->GetSystemAddressFromGuid(destination)==UNASSIGNED_SYSTEM_ADDRESS);

    if( natPunchthroughDebugInterface )
    {
        char guidString[128];
        destination.ToString( guidString );
        natPunchthroughDebugInterface->OnClientMessage( RakNet::format( "Starting ID_NAT_PUNCHTHROUGH_REQUEST to guid %s.", guidString ).c_str() );
    }
}
void NatPunchthroughClient::OnAttach( void )
{
    Clear();
}
void NatPunchthroughClient::OnDetach( void )
{
    Clear();
}
void NatPunchthroughClient::OnRakPeerShutdown( void )
{
    Clear();
}
void NatPunchthroughClient::Clear( void )
{
    OnReadyForNextPunchthrough();

    failedAttemptList.clear();
    queuedOpenNat.clear();
}
PunchthroughConfiguration* NatPunchthroughClient::GetPunchthroughConfiguration( void )
{
    return &pc;
}
void NatPunchthroughClient::OnReadyForNextPunchthrough( void )
{
    if( rakPeerInterface == 0 )
        return;

    sp.nextActionTime = 0;

    BitStream outgoingBs;
    outgoingBs.Write( (MessageID)ID_NAT_CLIENT_READY );
    rakPeerInterface->Send( &outgoingBs, HIGH_PRIORITY, RELIABLE_ORDERED, 0, sp.facilitator, false );
}

void NatPunchthroughClient::PushSuccess( void )
{
    Packet* p = AllocatePacketUnified( sizeof( MessageID ) + sizeof( unsigned char ) );
    p->data[0] = ID_NAT_PUNCHTHROUGH_SUCCEEDED;
    p->systemAddress = sp.targetAddress;
    p->systemAddress.systemIndex = (SystemIndex)-1;
    p->guid = sp.targetGuid;
    if( sp.weAreSender )
        p->data[1] = 1;
    else
        p->data[1] = 0;
    p->wasGeneratedLocally = true;
    rakPeerInterface->PushBackPacket( p, true );
}
bool NatPunchthroughClient::RemoveFromFailureQueue( void )
{
    auto it = std::find_if( failedAttemptList.begin(), failedAttemptList.end(),
                            [&]( const AddrAndGuid& rAddr ) { return rAddr.guid == sp.targetGuid; } );
    if( it != failedAttemptList.end() )
    {
        // Remove from failure queue
        failedAttemptList.erase( it );
        return true;
    }
    return false;
}

void NatPunchthroughClient::IncrementExternalAttemptCount( RakNet::Time time, RakNet::Time delta )
{
    if( ++sp.retryCount >= pc.UDP_SENDS_PER_PORT_EXTERNAL )
    {
        ++sp.attemptCount;
        sp.retryCount = 0;
        sp.nextActionTime = time + pc.EXTERNAL_IP_WAIT_BETWEEN_PORTS - delta;
        sp.sentTTL = false;
    }
    else
    {
        sp.nextActionTime = time + pc.TIME_BETWEEN_PUNCH_ATTEMPTS_EXTERNAL - delta;
    }
}

} // namespace RakNet

#endif // _RAKNET_SUPPORT_*
