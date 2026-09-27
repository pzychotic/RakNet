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
#if _RAKNET_SUPPORT_UDPProxyClient == 1

#include "Plugins/UDPProxyClient.h"
#include "Plugins/UDPProxyCommon.h"
#include "BitStream.h"
#include "RakPeerInterface.h"
#include "MessageIdentifiers.h"
#include "GetTime.h"

#include <algorithm>
#include <string>

namespace RakNet {

static const int DEFAULT_UNRESPONSIVE_PING_TIME_COORDINATOR = 1000;

// bool operator<( const DataStructures::MLKeyRef<UDPProxyClient::ServerWithPing> &inputKey, const UDPProxyClient::ServerWithPing &cls ) {return inputKey.Get().serverAddress < cls.serverAddress;}
// bool operator>( const DataStructures::MLKeyRef<UDPProxyClient::ServerWithPing> &inputKey, const UDPProxyClient::ServerWithPing &cls ) {return inputKey.Get().serverAddress > cls.serverAddress;}
// bool operator==( const DataStructures::MLKeyRef<UDPProxyClient::ServerWithPing> &inputKey, const UDPProxyClient::ServerWithPing &cls ) {return inputKey.Get().serverAddress == cls.serverAddress;}

STATIC_FACTORY_DEFINITIONS( UDPProxyClient, UDPProxyClient );

UDPProxyClient::UDPProxyClient()
{
    resultHandler = 0;
}
UDPProxyClient::~UDPProxyClient()
{
    Clear();
}
void UDPProxyClient::SetResultHandler( UDPProxyClientResultHandler* rh )
{
    resultHandler = rh;
}
void UDPProxyClient::AddCoordinator( const SystemAddress& systemAddress )
{
    if( !IsDesignatedCoordinator( systemAddress ) )
        coordinators.push_back( systemAddress );
}
void UDPProxyClient::RemoveCoordinator( const SystemAddress& systemAddress )
{
    coordinators.erase( std::remove( coordinators.begin(), coordinators.end(), systemAddress ), coordinators.end() );
}
bool UDPProxyClient::IsDesignatedCoordinator( const SystemAddress& systemAddress ) const
{
    return std::find( coordinators.begin(), coordinators.end(), systemAddress ) != coordinators.end();
}
bool UDPProxyClient::AddOutstandingRequest( const OutstandingRequest& request )
{
    for( OutstandingRequest& existing : outstandingRequests )
    {
        if( existing.coordinatorAddress == request.coordinatorAddress &&
            existing.sourceAddress == request.sourceAddress &&
            existing.usesAddress == request.usesAddress &&
            ( request.usesAddress ? existing.targetAddress == request.targetAddress : existing.targetGuid == request.targetGuid ) )
        {
            // The coordinator answers a repeat with ID_UDP_PROXY_IN_PROGRESS, and the first request's final result retires both
            existing.requestTime = request.requestTime;
            existing.timeoutOnNoDataMS = request.timeoutOnNoDataMS;
            return true;
        }
    }
    if( outstandingRequests.size() >= MAX_OUTSTANDING_REQUESTS )
        return false;
    outstandingRequests.push_back( request );
    return true;
}
std::vector<UDPProxyClient::OutstandingRequest>::iterator UDPProxyClient::FindOutstandingRequest( const SystemAddress& coordinatorAddress, const SystemAddress& sourceAddress, const SystemAddress& targetAddress, RakNetGUID targetGuid )
{
    return std::find_if( outstandingRequests.begin(), outstandingRequests.end(), [&]( const OutstandingRequest& request ) {
        // The coordinator resolves the target the request did not name, so match only the one it did
        return request.coordinatorAddress == coordinatorAddress &&
               ( request.sourceAddress == UNASSIGNED_SYSTEM_ADDRESS || request.sourceAddress == sourceAddress ) &&
               ( request.usesAddress ? request.targetAddress == targetAddress : request.targetGuid == targetGuid );
    } );
}
bool UDPProxyClient::RequestForwarding( SystemAddress proxyCoordinator, SystemAddress sourceAddress, RakNetGUID targetGuid, RakNet::TimeMS timeoutOnNoDataMS, BitStream* serverSelectionBitstream )
{
    // Return false if not connected
    ConnectionState cs = rakPeerInterface->GetConnectionState( proxyCoordinator );
    if( cs != IS_CONNECTED )
        return false;

    // Pretty much a bug not to set the result handler, as otherwise you won't know if the operation succeeed or not
    RakAssert( resultHandler != 0 );
    if( resultHandler == 0 )
        return false;

    OutstandingRequest request;
    request.coordinatorAddress = proxyCoordinator;
    request.sourceAddress = sourceAddress;
    request.usesAddress = false;
    request.targetAddress = UNASSIGNED_SYSTEM_ADDRESS;
    request.targetGuid = targetGuid;
    request.requestTime = RakNet::GetTimeMS();
    request.timeoutOnNoDataMS = timeoutOnNoDataMS;
    if( !AddOutstandingRequest( request ) )
        return false;

    BitStream outgoingBs;
    outgoingBs.Write( (MessageID)ID_UDP_PROXY_GENERAL );
    outgoingBs.Write( (MessageID)ID_UDP_PROXY_FORWARDING_REQUEST_FROM_CLIENT_TO_COORDINATOR );
    outgoingBs.Write( sourceAddress );
    outgoingBs.Write( false );
    outgoingBs.Write( targetGuid );
    outgoingBs.Write( timeoutOnNoDataMS );
    if( serverSelectionBitstream && serverSelectionBitstream->GetNumberOfBitsUsed() > 0 )
    {
        outgoingBs.Write( true );
        outgoingBs.Write( serverSelectionBitstream );
    }
    else
    {
        outgoingBs.Write( false );
    }
    rakPeerInterface->Send( &outgoingBs, MEDIUM_PRIORITY, RELIABLE_ORDERED, 0, proxyCoordinator, false );

    return true;
}
bool UDPProxyClient::RequestForwarding( SystemAddress proxyCoordinator, SystemAddress sourceAddress, SystemAddress targetAddressAsSeenFromCoordinator, RakNet::TimeMS timeoutOnNoDataMS, BitStream* serverSelectionBitstream )
{
    // Return false if not connected
    ConnectionState cs = rakPeerInterface->GetConnectionState( proxyCoordinator );
    if( cs != IS_CONNECTED )
        return false;

    // Pretty much a bug not to set the result handler, as otherwise you won't know if the operation succeeed or not
    RakAssert( resultHandler != 0 );
    if( resultHandler == 0 )
        return false;

    OutstandingRequest request;
    request.coordinatorAddress = proxyCoordinator;
    request.sourceAddress = sourceAddress;
    request.usesAddress = true;
    request.targetAddress = targetAddressAsSeenFromCoordinator;
    request.targetGuid = UNASSIGNED_RAKNET_GUID;
    request.requestTime = RakNet::GetTimeMS();
    request.timeoutOnNoDataMS = timeoutOnNoDataMS;
    if( !AddOutstandingRequest( request ) )
        return false;

    BitStream outgoingBs;
    outgoingBs.Write( (MessageID)ID_UDP_PROXY_GENERAL );
    outgoingBs.Write( (MessageID)ID_UDP_PROXY_FORWARDING_REQUEST_FROM_CLIENT_TO_COORDINATOR );
    outgoingBs.Write( sourceAddress );
    outgoingBs.Write( true );
    outgoingBs.Write( targetAddressAsSeenFromCoordinator );
    outgoingBs.Write( timeoutOnNoDataMS );
    if( serverSelectionBitstream && serverSelectionBitstream->GetNumberOfBitsUsed() > 0 )
    {
        outgoingBs.Write( true );
        outgoingBs.Write( serverSelectionBitstream );
    }
    else
    {
        outgoingBs.Write( false );
    }
    rakPeerInterface->Send( &outgoingBs, MEDIUM_PRIORITY, RELIABLE_ORDERED, 0, proxyCoordinator, false );

    return true;
}
void UDPProxyClient::Update( void )
{
    const RakNet::TimeMS curTime = RakNet::GetTimeMS();
    outstandingRequests.erase( std::remove_if( outstandingRequests.begin(), outstandingRequests.end(), [curTime]( const OutstandingRequest& request ) {
                                   return (RakNet::TimeMS)( curTime - request.requestTime ) > request.timeoutOnNoDataMS;
                               } ),
                               outstandingRequests.end() );

    for( auto it = pingServerGroups.begin(); it != pingServerGroups.end(); /**/ )
    {
        PingServerGroup* psg = *it;

        if( !psg->serversToPing.empty() &&
            RakNet::GetTimeMS() > psg->startPingTime + DEFAULT_UNRESPONSIVE_PING_TIME_COORDINATOR )
        {
            // If they didn't reply within DEFAULT_UNRESPONSIVE_PING_TIME_COORDINATOR, just give up on them
            psg->SendPingedServersToCoordinator( rakPeerInterface );

            RakNet::OP_DELETE( psg, _FILE_AND_LINE_ );
            it = pingServerGroups.erase( it );
        }
        else
        {
            ++it;
        }
    }
}
PluginReceiveResult UDPProxyClient::OnReceive( Packet* packet )
{
    if( packet->data[0] == ID_UNCONNECTED_PONG )
    {
        for( auto it = pingServerGroups.begin(); it != pingServerGroups.end(); ++it )
        {
            PingServerGroup* psg = *it;

            for( ServerWithPing& rServer : psg->serversToPing )
            {
                if( rServer.serverAddress == packet->systemAddress )
                {
                    BitStream bsIn( packet->data, packet->length, false );
                    bsIn.IgnoreBytes( sizeof( MessageID ) );
                    RakNet::TimeMS sentTime;
                    bsIn.Read( sentTime );
                    RakNet::TimeMS curTime = RakNet::GetTimeMS();
                    int ping =  curTime > sentTime ? (int)(curTime - sentTime) : 0;
                    rServer.ping = (unsigned short)ping;

                    // If all servers to ping are now pinged, reply to coordinator
                    if( psg->AreAllServersPinged() )
                    {
                        psg->SendPingedServersToCoordinator( rakPeerInterface );
                        RakNet::OP_DELETE( psg, _FILE_AND_LINE_ );
                        pingServerGroups.erase( it );
                    }

                    return RR_STOP_PROCESSING_AND_DEALLOCATE;
                }
            }
        }
    }
    else if( packet->data[0] == ID_UDP_PROXY_GENERAL && packet->length > 1 )
    {
        switch( packet->data[1] )
        {
        case ID_UDP_PROXY_PING_SERVERS_FROM_COORDINATOR_TO_CLIENT: {
            if( IsDesignatedCoordinator( packet->systemAddress ) )
                OnPingServers( packet );
        }
        break;
        case ID_UDP_PROXY_FORWARDING_SUCCEEDED:
        case ID_UDP_PROXY_ALL_SERVERS_BUSY:
        case ID_UDP_PROXY_IN_PROGRESS:
        case ID_UDP_PROXY_NO_SERVERS_ONLINE:
        case ID_UDP_PROXY_RECIPIENT_GUID_NOT_CONNECTED_TO_COORDINATOR:
        case ID_UDP_PROXY_FORWARDING_NOTIFICATION: {
            RakNetGUID targetGuid;
            SystemAddress senderAddress, targetAddress;
            BitStream incomingBs( packet->data, packet->length, false );
            incomingBs.IgnoreBytes( sizeof( MessageID ) * 2 );
            if( !incomingBs.Read( senderAddress ) || !incomingBs.Read( targetAddress ) || !incomingBs.Read( targetGuid ) )
                return RR_STOP_PROCESSING_AND_DEALLOCATE;

            if( packet->data[1] == ID_UDP_PROXY_FORWARDING_NOTIFICATION )
            {
                // This Peer is the target and asked for nothing, so only a Designated coordinator may say forwarding is set up
                if( !IsDesignatedCoordinator( packet->systemAddress ) )
                    return RR_STOP_PROCESSING_AND_DEALLOCATE;
            }
            else
            {
                auto request = FindOutstandingRequest( packet->systemAddress, senderAddress, targetAddress, targetGuid );
                if( request == outstandingRequests.end() )
                    return RR_STOP_PROCESSING_AND_DEALLOCATE;
                // Retired before the callback, which may call RequestForwarding() again
                if( packet->data[1] != ID_UDP_PROXY_IN_PROGRESS )
                    outstandingRequests.erase( request );
            }

            switch( packet->data[1] )
            {
            case ID_UDP_PROXY_FORWARDING_NOTIFICATION:
            case ID_UDP_PROXY_FORWARDING_SUCCEEDED:
            case ID_UDP_PROXY_IN_PROGRESS: {
                unsigned short forwardingPort;
                std::string serverIP;
                incomingBs.Read( serverIP );
                incomingBs.Read( forwardingPort );
                if( packet->data[1] == ID_UDP_PROXY_FORWARDING_SUCCEEDED )
                {
                    if( resultHandler )
                        resultHandler->OnForwardingSuccess( serverIP.c_str(), forwardingPort, packet->systemAddress, senderAddress, targetAddress, targetGuid, this );
                }
                else if( packet->data[1] == ID_UDP_PROXY_IN_PROGRESS )
                {
                    if( resultHandler )
                        resultHandler->OnForwardingInProgress( serverIP.c_str(), forwardingPort, packet->systemAddress, senderAddress, targetAddress, targetGuid, this );
                }
                else
                {
                    // Send a datagram to the proxy, so if we are behind a router, that router adds an entry to the routing table.
                    // Otherwise the router would block the incoming datagrams from source
                    // It doesn't matter if the message actually arrives as long as it goes through the router
                    rakPeerInterface->Ping( serverIP.c_str(), forwardingPort, false );

                    if( resultHandler )
                        resultHandler->OnForwardingNotification( serverIP.c_str(), forwardingPort, packet->systemAddress, senderAddress, targetAddress, targetGuid, this );
                }
            }
            break;
            case ID_UDP_PROXY_ALL_SERVERS_BUSY:
                if( resultHandler )
                    resultHandler->OnAllServersBusy( packet->systemAddress, senderAddress, targetAddress, targetGuid, this );
                break;
            case ID_UDP_PROXY_NO_SERVERS_ONLINE:
                if( resultHandler )
                    resultHandler->OnNoServersOnline( packet->systemAddress, senderAddress, targetAddress, targetGuid, this );
                break;
            case ID_UDP_PROXY_RECIPIENT_GUID_NOT_CONNECTED_TO_COORDINATOR: {
                if( resultHandler )
                    resultHandler->OnRecipientNotConnected( packet->systemAddress, senderAddress, targetAddress, targetGuid, this );
                break;
            }
            }
        }
            return RR_STOP_PROCESSING_AND_DEALLOCATE;
        }
    }
    return RR_CONTINUE_PROCESSING;
}
void UDPProxyClient::OnRakPeerShutdown( void )
{
    Clear();
}
void UDPProxyClient::OnClosedConnection( const SystemAddress& systemAddress, RakNetGUID rakNetGUID, PI2_LostConnectionReason lostConnectionReason )
{
    (void)rakNetGUID;
    (void)lostConnectionReason;

    // A System that later connects from the same address inherits neither the designation nor the requests
    RemoveCoordinator( systemAddress );
    outstandingRequests.erase( std::remove_if( outstandingRequests.begin(), outstandingRequests.end(), [&systemAddress]( const OutstandingRequest& request ) {
                                   return request.coordinatorAddress == systemAddress;
                               } ),
                               outstandingRequests.end() );
}
void UDPProxyClient::OnPingServers( Packet* packet )
{
    BitStream incomingBs( packet->data, packet->length, false );
    incomingBs.IgnoreBytes( 2 );

    PingServerGroup* psg = RakNet::OP_NEW<PingServerGroup>( _FILE_AND_LINE_ );

    incomingBs.Read( psg->sata.senderClientAddress );
    incomingBs.Read( psg->sata.targetClientAddress );
    psg->startPingTime = RakNet::GetTimeMS();
    psg->coordinatorAddressForPings = packet->systemAddress;
    unsigned short serverListSize;
    incomingBs.Read( serverListSize );
    SystemAddress serverAddress;
    char ipStr[64];
    for( unsigned short serverListIndex = 0; serverListIndex < serverListSize; serverListIndex++ )
    {
        incomingBs.Read( serverAddress );
        psg->serversToPing.emplace_back( ServerWithPing{ DEFAULT_UNRESPONSIVE_PING_TIME_COORDINATOR, serverAddress } );
        serverAddress.ToString( false, ipStr );
        rakPeerInterface->Ping( ipStr, serverAddress.GetPort(), false, 0 );
    }
    pingServerGroups.push_back( psg );
}

bool UDPProxyClient::PingServerGroup::AreAllServersPinged( void ) const
{
    for( const ServerWithPing& rServer : serversToPing )
    {
        if( rServer.ping == DEFAULT_UNRESPONSIVE_PING_TIME_COORDINATOR )
        {
            return false;
        }
    }
    return true;
}

void UDPProxyClient::PingServerGroup::SendPingedServersToCoordinator( RakPeerInterface* rakPeerInterface )
{
    BitStream outgoingBs;
    outgoingBs.Write( (MessageID)ID_UDP_PROXY_GENERAL );
    outgoingBs.Write( (MessageID)ID_UDP_PROXY_PING_SERVERS_REPLY_FROM_CLIENT_TO_COORDINATOR );
    outgoingBs.Write( sata.senderClientAddress );
    outgoingBs.Write( sata.targetClientAddress );
    unsigned short serversToPingSize = (unsigned short)serversToPing.size();
    outgoingBs.Write( serversToPingSize );
    for( const ServerWithPing& rServer : serversToPing )
    {
        outgoingBs.Write( rServer.serverAddress );
        outgoingBs.Write( rServer.ping );
    }
    rakPeerInterface->Send( &outgoingBs, MEDIUM_PRIORITY, RELIABLE_ORDERED, 0, coordinatorAddressForPings, false );
}
void UDPProxyClient::Clear( void )
{
    for( PingServerGroup* pGroup : pingServerGroups )
    {
        RakNet::OP_DELETE( pGroup, _FILE_AND_LINE_ );
    }
    pingServerGroups.clear();
    coordinators.clear();
    outstandingRequests.clear();
}

} // namespace RakNet

#endif // _RAKNET_SUPPORT_*
