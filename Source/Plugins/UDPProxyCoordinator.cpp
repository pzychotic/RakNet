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
#if _RAKNET_SUPPORT_UDPProxyCoordinator == 1 && _RAKNET_SUPPORT_UDPForwarder == 1

#include "Plugins/UDPProxyCoordinator.h"
#include "Plugins/UDPProxyCommon.h"
#include "BitStream.h"
#include "RakPeerInterface.h"
#include "MessageIdentifiers.h"
#include "Rand.h"
#include "GetTime.h"
#include "UDPForwarder.h"

#include <algorithm>

namespace RakNet {

// Larger than the client version
static const int DEFAULT_CLIENT_UNRESPONSIVE_PING_TIME = 2000;
static const int DEFAULT_UNRESPONSIVE_PING_TIME_COORDINATOR = DEFAULT_CLIENT_UNRESPONSIVE_PING_TIME + 1000;

int UDPProxyCoordinator::ServerWithPingComp( const unsigned short& key, const UDPProxyCoordinator::ServerWithPing& data )
{
    if( key < data.ping )
        return -1;
    if( key > data.ping )
        return 1;
    return 0;
}

int UDPProxyCoordinator::ForwardingRequestComp( const SenderAndTargetAddress& key, ForwardingRequest* const& data )
{
    if( key.senderClientAddress < data->sata.senderClientAddress )
        return -1;
    if( key.senderClientAddress > data->sata.senderClientAddress )
        return 1;
    if( key.targetClientAddress < data->sata.targetClientAddress )
        return -1;
    if( key.targetClientAddress > data->sata.targetClientAddress )
        return 1;
    return 0;
}

STATIC_FACTORY_DEFINITIONS( UDPProxyCoordinator, UDPProxyCoordinator );

UDPProxyCoordinator::UDPProxyCoordinator()
{
}
UDPProxyCoordinator::~UDPProxyCoordinator()
{
    Clear();
}
void UDPProxyCoordinator::SetRemoteLoginPassword( const std::string& password )
{
    remoteLoginPassword = password;
}
void UDPProxyCoordinator::Update( void )
{
    unsigned int idx;
    RakNet::TimeMS curTime = RakNet::GetTimeMS();
    ForwardingRequest* fw;
    idx = 0;
    while( idx < forwardingRequestList.Size() )
    {
        fw = forwardingRequestList[idx];
        if( fw->timeRequestedPings != 0 &&
            curTime > fw->timeRequestedPings + DEFAULT_UNRESPONSIVE_PING_TIME_COORDINATOR )
        {
            fw->OrderRemainingServersToTry();
            fw->timeRequestedPings = 0;
            TryNextServer( fw->sata, fw );
            idx++;
        }
        else if( fw->timeoutAfterSuccess != 0 &&
                 curTime > fw->timeoutAfterSuccess )
        {
            // Forwarding request succeeded, we waited a bit to prevent duplicates. Can forget about the entry now.
            RakNet::OP_DELETE( fw, _FILE_AND_LINE_ );
            forwardingRequestList.RemoveAtIndex( idx );
        }
        else
            idx++;
    }
}
PluginReceiveResult UDPProxyCoordinator::OnReceive( Packet* packet )
{
    if( packet->data[0] == ID_UDP_PROXY_GENERAL && packet->length > 1 )
    {
        switch( packet->data[1] )
        {
        case ID_UDP_PROXY_FORWARDING_REQUEST_FROM_CLIENT_TO_COORDINATOR:
            OnForwardingRequestFromClientToCoordinator( packet );
            return RR_STOP_PROCESSING_AND_DEALLOCATE;
        case ID_UDP_PROXY_LOGIN_REQUEST_FROM_SERVER_TO_COORDINATOR:
            OnLoginRequestFromServerToCoordinator( packet );
            return RR_STOP_PROCESSING_AND_DEALLOCATE;
        case ID_UDP_PROXY_FORWARDING_REPLY_FROM_SERVER_TO_COORDINATOR:
            OnForwardingReplyFromServerToCoordinator( packet );
            return RR_STOP_PROCESSING_AND_DEALLOCATE;
        case ID_UDP_PROXY_PING_SERVERS_REPLY_FROM_CLIENT_TO_COORDINATOR:
            OnPingServersReplyFromClientToCoordinator( packet );
            return RR_STOP_PROCESSING_AND_DEALLOCATE;
        }
    }
    return RR_CONTINUE_PROCESSING;
}
void UDPProxyCoordinator::OnClosedConnection( const SystemAddress& systemAddress, RakNetGUID rakNetGUID, PI2_LostConnectionReason lostConnectionReason )
{
    (void)lostConnectionReason;
    (void)rakNetGUID;

    unsigned int idx = 0;
    while( idx < forwardingRequestList.Size() )
    {
        if( forwardingRequestList[idx]->requestingAddress == systemAddress )
        {
            // Guy disconnected before the attempt completed
            RakNet::OP_DELETE( forwardingRequestList[idx], _FILE_AND_LINE_ );
            forwardingRequestList.RemoveAtIndex( idx );
        }
        else
        {
            idx++;
        }
    }

    auto it = std::find( serverList.begin(), serverList.end(), systemAddress );
    if( it != serverList.end() )
    {
        // For each pending client for this server, choose from remaining servers.
        for( unsigned int idx2 = 0; idx2 < forwardingRequestList.Size(); idx2++ )
        {
            ForwardingRequest* fw = forwardingRequestList[idx2];
            if( fw->currentlyAttemptedServerAddress == systemAddress )
            {
                // Try the next server
                TryNextServer( fw->sata, fw );
            }
        }

        // Remove dead server
        serverList.erase( it );
    }
}
void UDPProxyCoordinator::OnForwardingRequestFromClientToCoordinator( Packet* packet )
{
    BitStream incomingBs( packet->data, packet->length, false );
    incomingBs.IgnoreBytes( 2 );
    SystemAddress sourceAddress;
    incomingBs.Read( sourceAddress );
    if( sourceAddress == UNASSIGNED_SYSTEM_ADDRESS )
        sourceAddress = packet->systemAddress;
    SystemAddress targetAddress;
    RakNetGUID targetGuid;
    bool usesAddress = false;
    incomingBs.Read( usesAddress );
    if( usesAddress )
    {
        incomingBs.Read( targetAddress );
        targetGuid = rakPeerInterface->GetGuidFromSystemAddress( targetAddress );
    }
    else
    {
        incomingBs.Read( targetGuid );
        targetAddress = rakPeerInterface->GetSystemAddressFromGuid( targetGuid );
    }
    ForwardingRequest* fw = RakNet::OP_NEW<ForwardingRequest>( _FILE_AND_LINE_ );
    fw->timeoutAfterSuccess = 0;
    incomingBs.Read( fw->timeoutOnNoDataMS );
    bool hasServerSelectionBitstream = false;
    incomingBs.Read( hasServerSelectionBitstream );
    if( hasServerSelectionBitstream )
        incomingBs.Read( &( fw->serverSelectionBitstream ) );

    BitStream outgoingBs;
    SenderAndTargetAddress sata;
    sata.senderClientAddress = sourceAddress;
    sata.targetClientAddress = targetAddress;
    sata.targetClientGuid = targetGuid;
    sata.senderClientGuid = rakPeerInterface->GetGuidFromSystemAddress( sourceAddress );
    SenderAndTargetAddress sataReversed;
    sataReversed.senderClientAddress = targetAddress;
    sataReversed.targetClientAddress = sourceAddress;
    sataReversed.senderClientGuid = sata.targetClientGuid;
    sataReversed.targetClientGuid = sata.senderClientGuid;

    unsigned int insertionIndex;
    bool objectExists1, objectExists2;
    insertionIndex = forwardingRequestList.GetIndexFromKey( sata, &objectExists1 );
    forwardingRequestList.GetIndexFromKey( sataReversed, &objectExists2 );

    if( objectExists1 || objectExists2 )
    {
        outgoingBs.Write( (MessageID)ID_UDP_PROXY_GENERAL );
        outgoingBs.Write( (MessageID)ID_UDP_PROXY_IN_PROGRESS );
        outgoingBs.Write( sata.senderClientAddress );
        outgoingBs.Write( targetAddress );
        outgoingBs.Write( targetGuid );
        // Request in progress, not completed
        unsigned short forwardingPort = 0;
        std::string serverPublicIp;
        outgoingBs.Write( serverPublicIp );
        outgoingBs.Write( forwardingPort );
        rakPeerInterface->Send( &outgoingBs, MEDIUM_PRIORITY, RELIABLE_ORDERED, 0, packet->systemAddress, false );
        RakNet::OP_DELETE( fw, _FILE_AND_LINE_ );
        return;
    }

    if( serverList.empty() )
    {
        outgoingBs.Write( (MessageID)ID_UDP_PROXY_GENERAL );
        outgoingBs.Write( (MessageID)ID_UDP_PROXY_NO_SERVERS_ONLINE );
        outgoingBs.Write( sata.senderClientAddress );
        outgoingBs.Write( targetAddress );
        outgoingBs.Write( targetGuid );
        rakPeerInterface->Send( &outgoingBs, MEDIUM_PRIORITY, RELIABLE_ORDERED, 0, packet->systemAddress, false );
        RakNet::OP_DELETE( fw, _FILE_AND_LINE_ );
        return;
    }

    if( rakPeerInterface->GetConnectionState( targetAddress ) != IS_CONNECTED && usesAddress == false )
    {
        outgoingBs.Write( (MessageID)ID_UDP_PROXY_GENERAL );
        outgoingBs.Write( (MessageID)ID_UDP_PROXY_RECIPIENT_GUID_NOT_CONNECTED_TO_COORDINATOR );
        outgoingBs.Write( sata.senderClientAddress );
        outgoingBs.Write( targetAddress );
        outgoingBs.Write( targetGuid );
        rakPeerInterface->Send( &outgoingBs, MEDIUM_PRIORITY, RELIABLE_ORDERED, 0, packet->systemAddress, false );
        RakNet::OP_DELETE( fw, _FILE_AND_LINE_ );
        return;
    }

    fw->sata = sata;
    fw->requestingAddress = packet->systemAddress;

    if( serverList.size() > 1 )
    {
        outgoingBs.Write( (MessageID)ID_UDP_PROXY_GENERAL );
        outgoingBs.Write( (MessageID)ID_UDP_PROXY_PING_SERVERS_FROM_COORDINATOR_TO_CLIENT );
        outgoingBs.Write( sourceAddress );
        outgoingBs.Write( targetAddress );
        outgoingBs.Write( targetGuid );
        unsigned short serverListSize = (unsigned short)serverList.size();
        outgoingBs.Write( serverListSize );
        for( const SystemAddress& rServer : serverList )
        {
            outgoingBs.Write( rServer );
        }
        rakPeerInterface->Send( &outgoingBs, MEDIUM_PRIORITY, RELIABLE_ORDERED, 0, sourceAddress, false );
        rakPeerInterface->Send( &outgoingBs, MEDIUM_PRIORITY, RELIABLE_ORDERED, 0, targetAddress, false );
        fw->timeRequestedPings = RakNet::GetTimeMS();
        for( const SystemAddress& rServer : serverList )
        {
            fw->remainingServersToTry.push_back( rServer );
        }
        forwardingRequestList.InsertAtIndex( fw, insertionIndex, _FILE_AND_LINE_ );
    }
    else
    {
        fw->timeRequestedPings = 0;
        fw->currentlyAttemptedServerAddress = serverList[0];
        forwardingRequestList.InsertAtIndex( fw, insertionIndex, _FILE_AND_LINE_ );
        SendForwardingRequest( sourceAddress, targetAddress, fw->currentlyAttemptedServerAddress, fw->timeoutOnNoDataMS );
    }
}

void UDPProxyCoordinator::SendForwardingRequest( SystemAddress sourceAddress, SystemAddress targetAddress, SystemAddress serverAddress, RakNet::TimeMS timeoutOnNoDataMS )
{
    BitStream outgoingBs;
    // Send request to desired server
    outgoingBs.Write( (MessageID)ID_UDP_PROXY_GENERAL );
    outgoingBs.Write( (MessageID)ID_UDP_PROXY_FORWARDING_REQUEST_FROM_COORDINATOR_TO_SERVER );
    outgoingBs.Write( sourceAddress );
    outgoingBs.Write( targetAddress );
    outgoingBs.Write( timeoutOnNoDataMS );
    rakPeerInterface->Send( &outgoingBs, MEDIUM_PRIORITY, RELIABLE_ORDERED, 0, serverAddress, false );
}
void UDPProxyCoordinator::OnLoginRequestFromServerToCoordinator( Packet* packet )
{
    BitStream incomingBs( packet->data, packet->length, false );
    incomingBs.IgnoreBytes( 2 );
    std::string password;
    incomingBs.Read( password );
    BitStream outgoingBs;

    if( remoteLoginPassword.empty() )
    {
        outgoingBs.Write( (MessageID)ID_UDP_PROXY_GENERAL );
        outgoingBs.Write( (MessageID)ID_UDP_PROXY_NO_PASSWORD_SET_FROM_COORDINATOR_TO_SERVER );
        outgoingBs.Write( password );
        rakPeerInterface->Send( &outgoingBs, MEDIUM_PRIORITY, RELIABLE_ORDERED, 0, packet->systemAddress, false );
        return;
    }

    if( remoteLoginPassword != password )
    {
        outgoingBs.Write( (MessageID)ID_UDP_PROXY_GENERAL );
        outgoingBs.Write( (MessageID)ID_UDP_PROXY_WRONG_PASSWORD_FROM_COORDINATOR_TO_SERVER );
        outgoingBs.Write( password );
        rakPeerInterface->Send( &outgoingBs, MEDIUM_PRIORITY, RELIABLE_ORDERED, 0, packet->systemAddress, false );
        return;
    }

    if( std::find( serverList.begin(), serverList.end(), packet->systemAddress ) != serverList.end() )
    {
        outgoingBs.Write( (MessageID)ID_UDP_PROXY_GENERAL );
        outgoingBs.Write( (MessageID)ID_UDP_PROXY_ALREADY_LOGGED_IN_FROM_COORDINATOR_TO_SERVER );
        outgoingBs.Write( password );
        rakPeerInterface->Send( &outgoingBs, MEDIUM_PRIORITY, RELIABLE_ORDERED, 0, packet->systemAddress, false );
        return;
    }
    serverList.emplace_back( packet->systemAddress );
    outgoingBs.Write( (MessageID)ID_UDP_PROXY_GENERAL );
    outgoingBs.Write( (MessageID)ID_UDP_PROXY_LOGIN_SUCCESS_FROM_COORDINATOR_TO_SERVER );
    outgoingBs.Write( password );
    rakPeerInterface->Send( &outgoingBs, MEDIUM_PRIORITY, RELIABLE_ORDERED, 0, packet->systemAddress, false );
}

void UDPProxyCoordinator::OnForwardingReplyFromServerToCoordinator( Packet* packet )
{
    BitStream incomingBs( packet->data, packet->length, false );
    incomingBs.IgnoreBytes( 2 );
    SenderAndTargetAddress sata;
    incomingBs.Read( sata.senderClientAddress );
    incomingBs.Read( sata.targetClientAddress );
    bool objectExists;
    unsigned int index = forwardingRequestList.GetIndexFromKey( sata, &objectExists );
    if( objectExists == false )
    {
        // The guy disconnected before the request finished
        return;
    }
    ForwardingRequest* fw = forwardingRequestList[index];
    sata.senderClientGuid = fw->sata.senderClientGuid;
    sata.targetClientGuid = fw->sata.targetClientGuid;

    std::string serverPublicIp;
    incomingBs.Read( serverPublicIp );

    if( serverPublicIp.empty() )
    {
        char serverIP[64];
        packet->systemAddress.ToString( false, serverIP );
        serverPublicIp = serverIP;
    }

    UDPForwarderResult success;
    unsigned char c;
    incomingBs.Read( c );
    success = (UDPForwarderResult)c;

    unsigned short forwardingPort;
    incomingBs.Read( forwardingPort );

    BitStream outgoingBs;
    if( success == UDPFORWARDER_SUCCESS )
    {
        outgoingBs.Write( (MessageID)ID_UDP_PROXY_GENERAL );
        outgoingBs.Write( (MessageID)ID_UDP_PROXY_FORWARDING_SUCCEEDED );
        outgoingBs.Write( sata.senderClientAddress );
        outgoingBs.Write( sata.targetClientAddress );
        outgoingBs.Write( sata.targetClientGuid );
        outgoingBs.Write( serverPublicIp );
        outgoingBs.Write( forwardingPort );
        rakPeerInterface->Send( &outgoingBs, MEDIUM_PRIORITY, RELIABLE_ORDERED, 0, fw->requestingAddress, false );

        outgoingBs.Reset();
        outgoingBs.Write( (MessageID)ID_UDP_PROXY_GENERAL );
        outgoingBs.Write( (MessageID)ID_UDP_PROXY_FORWARDING_NOTIFICATION );
        outgoingBs.Write( sata.senderClientAddress );
        outgoingBs.Write( sata.targetClientAddress );
        outgoingBs.Write( sata.targetClientGuid );
        outgoingBs.Write( serverPublicIp );
        outgoingBs.Write( forwardingPort );
        rakPeerInterface->Send( &outgoingBs, MEDIUM_PRIORITY, RELIABLE_ORDERED, 0, sata.targetClientAddress, false );

        // 05/18/09 Keep the entry around for some time after success, so duplicates are reported if attempting forwarding from the target system before notification of success
        fw->timeoutAfterSuccess = RakNet::GetTimeMS() + fw->timeoutOnNoDataMS;
        // forwardingRequestList.RemoveAtIndex(index);
        // RakNet::OP_DELETE(fw,_FILE_AND_LINE_);

        return;
    }
    else if( success == UDPFORWARDER_NO_SOCKETS )
    {
        // Try next server
        TryNextServer( sata, fw );
    }
    else
    {
        RakAssert( success == UDPFORWARDER_FORWARDING_ALREADY_EXISTS );

        // Return in progress
        outgoingBs.Write( (MessageID)ID_UDP_PROXY_GENERAL );
        outgoingBs.Write( (MessageID)ID_UDP_PROXY_IN_PROGRESS );
        outgoingBs.Write( sata.senderClientAddress );
        outgoingBs.Write( sata.targetClientAddress );
        outgoingBs.Write( sata.targetClientGuid );
        outgoingBs.Write( serverPublicIp );
        outgoingBs.Write( forwardingPort );
        rakPeerInterface->Send( &outgoingBs, MEDIUM_PRIORITY, RELIABLE_ORDERED, 0, fw->requestingAddress, false );
        forwardingRequestList.RemoveAtIndex( index );
        RakNet::OP_DELETE( fw, _FILE_AND_LINE_ );
    }
}
void UDPProxyCoordinator::OnPingServersReplyFromClientToCoordinator( Packet* packet )
{
    BitStream incomingBs( packet->data, packet->length, false );
    incomingBs.IgnoreBytes( 2 );
    unsigned short serversToPingSize;
    SystemAddress serverAddress;
    SenderAndTargetAddress sata;
    incomingBs.Read( sata.senderClientAddress );
    incomingBs.Read( sata.targetClientAddress );
    bool objectExists;
    unsigned int index = forwardingRequestList.GetIndexFromKey( sata, &objectExists );
    if( objectExists == false )
        return;
    ServerWithPing swp;
    ForwardingRequest* fw = forwardingRequestList[index];
    if( fw->timeRequestedPings == 0 )
        return;

    incomingBs.Read( serversToPingSize );
    if( packet->systemAddress == sata.senderClientAddress )
    {
        for( unsigned short idx = 0; idx < serversToPingSize; idx++ )
        {
            incomingBs.Read( swp.serverAddress );
            incomingBs.Read( swp.ping );

            auto it = fw->sourceServerPings.begin();
            while( it != fw->sourceServerPings.end() && it->ping < swp.ping )
            {
                ++it;
            }
            fw->sourceServerPings.insert( it, swp );
        }
    }
    else
    {
        for( unsigned short idx = 0; idx < serversToPingSize; idx++ )
        {
            incomingBs.Read( swp.serverAddress );
            incomingBs.Read( swp.ping );

            auto it = fw->targetServerPings.begin();
            while( it != fw->targetServerPings.end() && it->ping < swp.ping )
            {
                ++it;
            }
            fw->targetServerPings.insert( it, swp );
        }
    }

    // Both systems have to give us pings to progress here. Otherwise will timeout in Update()
    if( !fw->sourceServerPings.empty() && !fw->targetServerPings.empty() )
    {
        fw->OrderRemainingServersToTry();
        fw->timeRequestedPings = 0;
        TryNextServer( fw->sata, fw );
    }
}
void UDPProxyCoordinator::TryNextServer( SenderAndTargetAddress sata, ForwardingRequest* fw )
{
    bool pickedGoodServer = false;
    while( !fw->remainingServersToTry.empty() )
    {
        fw->currentlyAttemptedServerAddress = fw->remainingServersToTry.front();
        fw->remainingServersToTry.pop_front();
        if( std::find( serverList.begin(), serverList.end(), fw->currentlyAttemptedServerAddress ) != serverList.end() )
        {
            pickedGoodServer = true;
            break;
        }
    }

    if( pickedGoodServer == false )
    {
        SendAllBusy( sata.senderClientAddress, sata.targetClientAddress, sata.targetClientGuid, fw->requestingAddress );
        forwardingRequestList.Remove( sata );
        RakNet::OP_DELETE( fw, _FILE_AND_LINE_ );
        return;
    }

    SendForwardingRequest( sata.senderClientAddress, sata.targetClientAddress, fw->currentlyAttemptedServerAddress, fw->timeoutOnNoDataMS );
}
void UDPProxyCoordinator::SendAllBusy( SystemAddress senderClientAddress, SystemAddress targetClientAddress, RakNetGUID targetClientGuid, SystemAddress requestingAddress )
{
    BitStream outgoingBs;
    outgoingBs.Write( (MessageID)ID_UDP_PROXY_GENERAL );
    outgoingBs.Write( (MessageID)ID_UDP_PROXY_ALL_SERVERS_BUSY );
    outgoingBs.Write( senderClientAddress );
    outgoingBs.Write( targetClientAddress );
    outgoingBs.Write( targetClientGuid );
    rakPeerInterface->Send( &outgoingBs, MEDIUM_PRIORITY, RELIABLE_ORDERED, 0, requestingAddress, false );
}
void UDPProxyCoordinator::Clear( void )
{
    serverList.clear();
    for( unsigned int i = 0; i < forwardingRequestList.Size(); i++ )
    {
        RakNet::OP_DELETE( forwardingRequestList[i], _FILE_AND_LINE_ );
    }
    forwardingRequestList.Clear( false, _FILE_AND_LINE_ );
}
void UDPProxyCoordinator::ForwardingRequest::OrderRemainingServersToTry( void )
{
    DataStructures::OrderedList<unsigned short, UDPProxyCoordinator::ServerWithPing, ServerWithPingComp> swpList;

    if( sourceServerPings.empty() && targetServerPings.empty() )
        return;

    ServerWithPing swp;
    for( uint32_t idx = 0; idx < remainingServersToTry.size(); idx++ )
    {
        swp.serverAddress = remainingServersToTry[idx];
        swp.ping = 0;
        if( !sourceServerPings.empty() )
            swp.ping += (unsigned short)( sourceServerPings[idx].ping );
        else
            swp.ping += (unsigned short)( DEFAULT_CLIENT_UNRESPONSIVE_PING_TIME );
        if( !targetServerPings.empty() )
            swp.ping += (unsigned short)( targetServerPings[idx].ping );
        else
            swp.ping += (unsigned short)( DEFAULT_CLIENT_UNRESPONSIVE_PING_TIME );
        swpList.Insert( swp.ping, swp, false, _FILE_AND_LINE_ );
    }
    remainingServersToTry.clear();
    for( uint32_t idx = 0; idx < swpList.Size(); idx++ )
    {
        remainingServersToTry.push_back( swpList[idx].serverAddress );
    }
}

} // namespace RakNet

#endif // _RAKNET_SUPPORT_*
