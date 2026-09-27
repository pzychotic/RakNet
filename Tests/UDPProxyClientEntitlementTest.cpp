#include "Plugins/UDPProxyClient.h"
#include "Plugins/UDPProxyCommon.h"

#include "BitStream.h"
#include "ConnectionWaits.h"
#include "GetTime.h"
#include "MessageIdentifiers.h"
#include "PeerScope.h"
#include "RakNetStringMakers.h"
#include "RakNetTypes.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

/*
UDPProxyClient acts on a coordinator's message only if it is Solicited or comes from a
Designated coordinator (ADR-0006).

Ping-servers and forwarding notifications reach a Peer that may have asked for nothing, so
they are acted on only from a System designated with AddCoordinator. A result is acted on
only if it answers a request this Peer made, from the coordinator it asked. Stock took all of
them from any connected System: a ping-servers Message made the Peer ping every address it
listed, and a notification made it ping an address of the sender's choosing.

Each injected Message is followed by a user Message on the same ordered channel, so once the
user Message comes out of Receive the injected one has been through the plugin.
*/

using namespace RakNet;

namespace {

constexpr unsigned short kClientPort = 60000;
constexpr unsigned short kCoordinatorPort = 60001;
constexpr unsigned short kListenerPort = 60003;

// Hang guard for the marker Message and for a pong. On loopback each arrives a few update
// cycles after the send, tens of milliseconds.
constexpr TimeMS kMarkerBudgetMs = 5000;

// How long a test watches for a pong that must not come. A ping to loopback is answered
// within a few update cycles.
constexpr TimeMS kNoPongWindowMs = 300;

constexpr MessageID kMarker = ID_USER_PACKET_ENUM;

constexpr TimeMS kRequestTimeoutMs = 10000;

const RakNetGUID kTargetGuid( 2001 );
const SystemAddress kTargetAddress( "10.0.1.2", 2002 );
const SystemAddress kServerAddress( "10.0.0.1", 1001 );

struct RecordingHandler : public UDPProxyClientResultHandler
{
    int successes = 0;
    int notifications = 0;
    int noServersOnline = 0;
    int recipientNotConnected = 0;
    int allServersBusy = 0;
    int inProgress = 0;

    int Total() const
    {
        return successes + notifications + noServersOnline + recipientNotConnected + allServersBusy + inProgress;
    }

    void OnForwardingSuccess( const char*, unsigned short, SystemAddress, SystemAddress, SystemAddress, RakNetGUID, UDPProxyClient* ) override
    {
        successes++;
    }
    void OnForwardingNotification( const char*, unsigned short, SystemAddress, SystemAddress, SystemAddress, RakNetGUID, UDPProxyClient* ) override
    {
        notifications++;
    }
    void OnNoServersOnline( SystemAddress, SystemAddress, SystemAddress, RakNetGUID, UDPProxyClient* ) override
    {
        noServersOnline++;
    }
    void OnRecipientNotConnected( SystemAddress, SystemAddress, SystemAddress, RakNetGUID, UDPProxyClient* ) override
    {
        recipientNotConnected++;
    }
    void OnAllServersBusy( SystemAddress, SystemAddress, SystemAddress, RakNetGUID, UDPProxyClient* ) override
    {
        allServersBusy++;
    }
    void OnForwardingInProgress( const char*, unsigned short, SystemAddress, SystemAddress, SystemAddress, RakNetGUID, UDPProxyClient* ) override
    {
        inProgress++;
    }
};

// Receives until the marker comes out, and reports whether an ID_UNCONNECTED_PONG came
// out before it.
bool ReceiveUntilMarker( RakPeerInterface* peer, bool* sawPong )
{
    const TimeMS deadline = GetTimeMS() + kMarkerBudgetMs;
    while( !ConnectionWaits::Expired( deadline ) )
    {
        for( Packet* packet = peer->Receive(); packet != nullptr; packet = peer->Receive() )
        {
            const MessageID id = packet->data[0];
            peer->DeallocatePacket( packet );
            if( id == ID_UNCONNECTED_PONG && sawPong != nullptr )
                *sawPong = true;
            if( id == kMarker )
                return true;
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( ConnectionWaits::kPollInterval ) );
    }
    return false;
}

// Receives for up to budget, and reports whether an ID_UNCONNECTED_PONG came out.
bool PongWithin( RakPeerInterface* peer, TimeMS budget )
{
    const TimeMS deadline = GetTimeMS() + budget;
    while( !ConnectionWaits::Expired( deadline ) )
    {
        for( Packet* packet = peer->Receive(); packet != nullptr; packet = peer->Receive() )
        {
            const bool pong = packet->data[0] == ID_UNCONNECTED_PONG;
            peer->DeallocatePacket( packet );
            if( pong )
                return true;
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( ConnectionWaits::kPollInterval ) );
    }
    return false;
}

// Receives until Receive hands out id, which is after every plugin has seen it.
bool WaitForMessage( RakPeerInterface* peer, MessageID id )
{
    const TimeMS deadline = GetTimeMS() + kMarkerBudgetMs;
    while( !ConnectionWaits::Expired( deadline ) )
    {
        for( Packet* packet = peer->Receive(); packet != nullptr; packet = peer->Receive() )
        {
            const bool found = packet->data[0] == id;
            peer->DeallocatePacket( packet );
            if( found )
                return true;
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( ConnectionWaits::kPollInterval ) );
    }
    return false;
}

// Sends message from sender to target, then the marker, and receives on target until the
// marker comes out. Returns whether an ID_UNCONNECTED_PONG came out on the way.
bool Inject( RakPeerInterface* sender, RakPeerInterface* target, BitStream& message )
{
    const SystemAddress targetAddress = sender->GetSystemAddressFromGuid( target->GetMyGUID() );
    sender->Send( &message, HIGH_PRIORITY, RELIABLE_ORDERED, 0, targetAddress, false );

    BitStream marker;
    marker.Write( kMarker );
    sender->Send( &marker, HIGH_PRIORITY, RELIABLE_ORDERED, 0, targetAddress, false );

    bool sawPong = false;
    REQUIRE( ReceiveUntilMarker( target, &sawPong ) );
    ConnectionWaits::Drain( sender );
    return sawPong;
}

// ID_UDP_PROXY_PING_SERVERS_FROM_COORDINATOR_TO_CLIENT, laid out as UDPProxyCoordinator
// writes it.
void WritePingServers( BitStream& bs, const SystemAddress& server )
{
    bs.Write( (MessageID)ID_UDP_PROXY_GENERAL );
    bs.Write( (MessageID)ID_UDP_PROXY_PING_SERVERS_FROM_COORDINATOR_TO_CLIENT );
    bs.Write( UNASSIGNED_SYSTEM_ADDRESS );
    bs.Write( kTargetAddress );
    bs.Write( kTargetGuid );
    bs.Write( (unsigned short)1 );
    bs.Write( server );
}

// A result or notification, laid out as UDPProxyCoordinator writes it. Only the ones that
// name a proxy carry serverIP and port.
void WriteResult( BitStream& bs, MessageID resultId, const SystemAddress& source, const SystemAddress& target, RakNetGUID targetGuid, unsigned short proxyPort = kListenerPort )
{
    bs.Write( (MessageID)ID_UDP_PROXY_GENERAL );
    bs.Write( resultId );
    bs.Write( source );
    bs.Write( target );
    bs.Write( targetGuid );
    if( resultId == ID_UDP_PROXY_FORWARDING_SUCCEEDED || resultId == ID_UDP_PROXY_IN_PROGRESS || resultId == ID_UDP_PROXY_FORWARDING_NOTIFICATION )
    {
        bs.Write( std::string( "127.0.0.1" ) );
        bs.Write( proxyPort );
    }
}

void Connect( RakPeerInterface* client, RakPeerInterface* server )
{
    REQUIRE( client->Connect( "127.0.0.1", kClientPort, nullptr, 0 ) == CONNECTION_ATTEMPT_STARTED );
    const TimeMS deadline = GetTimeMS() + ConnectionWaits::kConnectionCountBudget;
    while( server->GetConnectionState( client->GetMyGUID() ) != IS_CONNECTED ||
           client->GetConnectionState( server->GetMyGUID() ) != IS_CONNECTED )
    {
        REQUIRE( !ConnectionWaits::Expired( deadline ) );
        ConnectionWaits::Drain( server );
        ConnectionWaits::Drain( client );
        std::this_thread::sleep_for( std::chrono::milliseconds( ConnectionWaits::kPollInterval ) );
    }
    ConnectionWaits::Drain( server );
}

SystemAddress Loopback( unsigned short port )
{
    return SystemAddress( "127.0.0.1", port );
}

} // namespace

TEST_CASE( "UDPProxyClient pings only for a Designated coordinator", "[udpproxy][network]" )
{
    // Before the PeerScope, so they outlive the peers.
    UDPProxyClient proxyClient;
    RecordingHandler handler;
    proxyClient.SetResultHandler( &handler );

    PeerScope peers;
    RakPeerInterface* client = peers.Server( kClientPort, 4 );
    RakPeerInterface* coordinator = peers.Client( kCoordinatorPort );
    RakPeerInterface* other = peers.Client();
    // Answers pings, so a ping the client sends to it comes back as a pong.
    peers.Client( kListenerPort );
    client->AttachPlugin( &proxyClient );

    Connect( coordinator, client );
    Connect( other, client );
    const SystemAddress coordinatorAddress = client->GetSystemAddressFromGuid( coordinator->GetMyGUID() );

    SECTION( "With nothing designated, nothing is acted on" )
    {
        BitStream pingServers;
        WritePingServers( pingServers, Loopback( kListenerPort ) );
        Inject( coordinator, client, pingServers );
        CHECK( proxyClient.pingServerGroups.empty() );

        BitStream notification;
        WriteResult( notification, ID_UDP_PROXY_FORWARDING_NOTIFICATION, kTargetAddress, Loopback( kClientPort ), kTargetGuid );
        const bool pongBeforeMarker = Inject( coordinator, client, notification );
        CHECK( handler.notifications == 0 );
        CHECK( !pongBeforeMarker );
        CHECK( !PongWithin( client, kNoPongWindowMs ) );
    }

    SECTION( "A Designated coordinator is acted on, and no one else" )
    {
        proxyClient.AddCoordinator( coordinatorAddress );

        BitStream forgedPingServers;
        WritePingServers( forgedPingServers, Loopback( kListenerPort ) );
        Inject( other, client, forgedPingServers );
        CHECK( proxyClient.pingServerGroups.empty() );

        BitStream pingServers;
        WritePingServers( pingServers, Loopback( kListenerPort ) );
        Inject( coordinator, client, pingServers );
        CHECK( proxyClient.pingServerGroups.size() == 1 );

        BitStream forgedNotification;
        WriteResult( forgedNotification, ID_UDP_PROXY_FORWARDING_NOTIFICATION, kTargetAddress, Loopback( kClientPort ), kTargetGuid );
        const bool forgedPong = Inject( other, client, forgedNotification );
        CHECK( handler.notifications == 0 );
        CHECK( !forgedPong );
        CHECK( !PongWithin( client, kNoPongWindowMs ) );

        BitStream notification;
        WriteResult( notification, ID_UDP_PROXY_FORWARDING_NOTIFICATION, kTargetAddress, Loopback( kClientPort ), kTargetGuid );
        const bool pong = Inject( coordinator, client, notification );
        CHECK( handler.notifications == 1 );
        CHECK( ( pong || PongWithin( client, kMarkerBudgetMs ) ) );
    }

    SECTION( "RemoveCoordinator withdraws the designation" )
    {
        proxyClient.AddCoordinator( coordinatorAddress );
        proxyClient.RemoveCoordinator( coordinatorAddress );

        BitStream pingServers;
        WritePingServers( pingServers, Loopback( kListenerPort ) );
        Inject( coordinator, client, pingServers );
        CHECK( proxyClient.pingServerGroups.empty() );

        BitStream notification;
        WriteResult( notification, ID_UDP_PROXY_FORWARDING_NOTIFICATION, kTargetAddress, Loopback( kClientPort ), kTargetGuid );
        Inject( coordinator, client, notification );
        CHECK( handler.notifications == 0 );
    }

    SECTION( "A designation lapses when its connection closes" )
    {
        proxyClient.AddCoordinator( coordinatorAddress );

        coordinator->CloseConnection( Loopback( kClientPort ), true );
        ConnectionWaits::WaitForDisconnect( coordinator, Loopback( kClientPort ) );
        REQUIRE( WaitForMessage( client, ID_DISCONNECTION_NOTIFICATION ) );

        // The same address again, but not the same designation.
        Connect( coordinator, client );
        REQUIRE( client->GetSystemAddressFromGuid( coordinator->GetMyGUID() ) == coordinatorAddress );

        BitStream pingServers;
        WritePingServers( pingServers, Loopback( kListenerPort ) );
        Inject( coordinator, client, pingServers );
        CHECK( proxyClient.pingServerGroups.empty() );

        BitStream notification;
        WriteResult( notification, ID_UDP_PROXY_FORWARDING_NOTIFICATION, kTargetAddress, Loopback( kClientPort ), kTargetGuid );
        Inject( coordinator, client, notification );
        CHECK( handler.notifications == 0 );
    }

    client->DetachPlugin( &proxyClient );
}

TEST_CASE( "UDPProxyClient takes a result only for a request it made, from the coordinator it asked", "[udpproxy][network]" )
{
    UDPProxyClient proxyClient;
    RecordingHandler handler;
    proxyClient.SetResultHandler( &handler );

    PeerScope peers;
    RakPeerInterface* client = peers.Server( kClientPort, 4 );
    RakPeerInterface* coordinator = peers.Client( kCoordinatorPort );
    RakPeerInterface* other = peers.Client();
    client->AttachPlugin( &proxyClient );

    Connect( coordinator, client );
    Connect( other, client );
    const SystemAddress coordinatorAddress = client->GetSystemAddressFromGuid( coordinator->GetMyGUID() );
    // What the coordinator writes for a source passed as UNASSIGNED_SYSTEM_ADDRESS.
    const SystemAddress requester = coordinator->GetSystemAddressFromGuid( client->GetMyGUID() );

    SECTION( "A result nothing asked for fires nothing" )
    {
        for( MessageID resultId : { ID_UDP_PROXY_FORWARDING_SUCCEEDED, ID_UDP_PROXY_IN_PROGRESS, ID_UDP_PROXY_ALL_SERVERS_BUSY,
                                    ID_UDP_PROXY_NO_SERVERS_ONLINE, ID_UDP_PROXY_RECIPIENT_GUID_NOT_CONNECTED_TO_COORDINATOR } )
        {
            BitStream result;
            WriteResult( result, resultId, requester, kTargetAddress, kTargetGuid );
            Inject( coordinator, client, result );
        }
        CHECK( handler.Total() == 0 );
    }

    SECTION( "A result from a System other than the coordinator asked fires nothing" )
    {
        REQUIRE( proxyClient.RequestForwarding( coordinatorAddress, UNASSIGNED_SYSTEM_ADDRESS, kTargetGuid, kRequestTimeoutMs ) );

        BitStream forged;
        WriteResult( forged, ID_UDP_PROXY_FORWARDING_SUCCEEDED, requester, kTargetAddress, kTargetGuid );
        Inject( other, client, forged );
        CHECK( handler.Total() == 0 );

        // The request is still outstanding.
        BitStream genuine;
        WriteResult( genuine, ID_UDP_PROXY_FORWARDING_SUCCEEDED, requester, kTargetAddress, kTargetGuid );
        Inject( coordinator, client, genuine );
        CHECK( handler.successes == 1 );
    }

    SECTION( "A result for a different target fires nothing" )
    {
        REQUIRE( proxyClient.RequestForwarding( coordinatorAddress, UNASSIGNED_SYSTEM_ADDRESS, kTargetGuid, kRequestTimeoutMs ) );

        BitStream otherGuid;
        WriteResult( otherGuid, ID_UDP_PROXY_FORWARDING_SUCCEEDED, requester, kTargetAddress, RakNetGUID( 2002 ) );
        Inject( coordinator, client, otherGuid );
        CHECK( handler.Total() == 0 );
    }

    SECTION( "A result for a different source fires nothing" )
    {
        const SystemAddress source( "10.0.2.1", 3001 );
        REQUIRE( proxyClient.RequestForwarding( coordinatorAddress, source, kTargetGuid, kRequestTimeoutMs ) );

        BitStream otherSource;
        WriteResult( otherSource, ID_UDP_PROXY_FORWARDING_SUCCEEDED, SystemAddress( "10.0.2.2", 3002 ), kTargetAddress, kTargetGuid );
        Inject( coordinator, client, otherSource );
        CHECK( handler.Total() == 0 );

        BitStream genuine;
        WriteResult( genuine, ID_UDP_PROXY_FORWARDING_SUCCEEDED, source, kTargetAddress, kTargetGuid );
        Inject( coordinator, client, genuine );
        CHECK( handler.successes == 1 );
    }

    SECTION( "A request by address matches on the address" )
    {
        REQUIRE( proxyClient.RequestForwarding( coordinatorAddress, UNASSIGNED_SYSTEM_ADDRESS, kTargetAddress, kRequestTimeoutMs ) );

        BitStream otherAddress;
        WriteResult( otherAddress, ID_UDP_PROXY_FORWARDING_SUCCEEDED, requester, SystemAddress( "10.0.1.3", 2003 ), UNASSIGNED_RAKNET_GUID );
        Inject( coordinator, client, otherAddress );
        CHECK( handler.Total() == 0 );

        BitStream genuine;
        WriteResult( genuine, ID_UDP_PROXY_FORWARDING_SUCCEEDED, requester, kTargetAddress, UNASSIGNED_RAKNET_GUID );
        Inject( coordinator, client, genuine );
        CHECK( handler.successes == 1 );
    }

    SECTION( "In progress keeps the request, and success retires it" )
    {
        REQUIRE( proxyClient.RequestForwarding( coordinatorAddress, UNASSIGNED_SYSTEM_ADDRESS, kTargetGuid, kRequestTimeoutMs ) );

        for( int i = 0; i < 2; i++ )
        {
            BitStream inProgress;
            WriteResult( inProgress, ID_UDP_PROXY_IN_PROGRESS, requester, kTargetAddress, kTargetGuid );
            Inject( coordinator, client, inProgress );
        }
        CHECK( handler.inProgress == 2 );

        for( int i = 0; i < 2; i++ )
        {
            BitStream success;
            WriteResult( success, ID_UDP_PROXY_FORWARDING_SUCCEEDED, requester, kTargetAddress, kTargetGuid );
            Inject( coordinator, client, success );
        }
        CHECK( handler.successes == 1 );
        CHECK( handler.Total() == 3 );
    }

    SECTION( "Each failure retires the request" )
    {
        for( MessageID resultId : { ID_UDP_PROXY_ALL_SERVERS_BUSY, ID_UDP_PROXY_NO_SERVERS_ONLINE, ID_UDP_PROXY_RECIPIENT_GUID_NOT_CONNECTED_TO_COORDINATOR } )
        {
            REQUIRE( proxyClient.RequestForwarding( coordinatorAddress, UNASSIGNED_SYSTEM_ADDRESS, kTargetGuid, kRequestTimeoutMs ) );
            for( int i = 0; i < 2; i++ )
            {
                BitStream result;
                WriteResult( result, resultId, requester, kTargetAddress, kTargetGuid );
                Inject( coordinator, client, result );
            }
        }
        CHECK( handler.allServersBusy == 1 );
        CHECK( handler.noServersOnline == 1 );
        CHECK( handler.recipientNotConnected == 1 );
        CHECK( handler.Total() == 3 );
    }

    SECTION( "A request lapses when the coordinator's connection closes" )
    {
        REQUIRE( proxyClient.RequestForwarding( coordinatorAddress, UNASSIGNED_SYSTEM_ADDRESS, kTargetGuid, kRequestTimeoutMs ) );

        coordinator->CloseConnection( Loopback( kClientPort ), true );
        ConnectionWaits::WaitForDisconnect( coordinator, Loopback( kClientPort ) );
        REQUIRE( WaitForMessage( client, ID_DISCONNECTION_NOTIFICATION ) );

        Connect( coordinator, client );
        REQUIRE( client->GetSystemAddressFromGuid( coordinator->GetMyGUID() ) == coordinatorAddress );

        BitStream result;
        WriteResult( result, ID_UDP_PROXY_FORWARDING_SUCCEEDED, requester, kTargetAddress, kTargetGuid );
        Inject( coordinator, client, result );
        CHECK( handler.Total() == 0 );
    }

    client->DetachPlugin( &proxyClient );
}

TEST_CASE( "UDPProxyClient times out and caps its outstanding requests", "[udpproxy][network]" )
{
    UDPProxyClient proxyClient;
    RecordingHandler handler;
    proxyClient.SetResultHandler( &handler );

    PeerScope peers;
    RakPeerInterface* client = peers.Server( kClientPort, 4 );
    RakPeerInterface* coordinator = peers.Client( kCoordinatorPort );
    client->AttachPlugin( &proxyClient );

    Connect( coordinator, client );
    const SystemAddress coordinatorAddress = client->GetSystemAddressFromGuid( coordinator->GetMyGUID() );
    const SystemAddress requester = coordinator->GetSystemAddressFromGuid( client->GetMyGUID() );

    SECTION( "A request is forgotten after timeoutOnNoDataMS" )
    {
        constexpr TimeMS kShortTimeoutMs = 50;
        REQUIRE( proxyClient.RequestForwarding( coordinatorAddress, UNASSIGNED_SYSTEM_ADDRESS, kTargetGuid, kShortTimeoutMs ) );
        std::this_thread::sleep_for( std::chrono::milliseconds( kShortTimeoutMs * 4 ) );

        BitStream late;
        WriteResult( late, ID_UDP_PROXY_FORWARDING_SUCCEEDED, requester, kTargetAddress, kTargetGuid );
        Inject( coordinator, client, late );
        CHECK( handler.Total() == 0 );
    }

    SECTION( "RequestForwarding fails at the cap until a request is retired" )
    {
        for( unsigned int i = 0; i < UDPProxyClient::MAX_OUTSTANDING_REQUESTS; i++ )
            REQUIRE( proxyClient.RequestForwarding( coordinatorAddress, UNASSIGNED_SYSTEM_ADDRESS, RakNetGUID( 3000 + i ), kRequestTimeoutMs ) );

        // Asking again for one already outstanding takes no new entry.
        CHECK( proxyClient.RequestForwarding( coordinatorAddress, UNASSIGNED_SYSTEM_ADDRESS, RakNetGUID( 3000 ), kRequestTimeoutMs ) );
        CHECK( !proxyClient.RequestForwarding( coordinatorAddress, UNASSIGNED_SYSTEM_ADDRESS, kTargetGuid, kRequestTimeoutMs ) );

        BitStream final;
        WriteResult( final, ID_UDP_PROXY_NO_SERVERS_ONLINE, requester, kTargetAddress, RakNetGUID( 3000 ) );
        Inject( coordinator, client, final );
        CHECK( handler.noServersOnline == 1 );
        CHECK( proxyClient.RequestForwarding( coordinatorAddress, UNASSIGNED_SYSTEM_ADDRESS, kTargetGuid, kRequestTimeoutMs ) );
    }

    client->DetachPlugin( &proxyClient );
}
