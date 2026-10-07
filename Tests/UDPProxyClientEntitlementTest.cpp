#include "Plugins/UDPProxyClient.h"
#include "Plugins/UDPProxyCommon.h"

#include "BitStream.h"
#include "ConnectionWaits.h"
#include "MarkerInjection.h"
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

Each forged Message is checked through MarkerInjection.
*/

using namespace RakNet;
using MarkerInjection::Contains;

namespace {

constexpr unsigned short kClientPort = 30000;
constexpr unsigned short kCoordinatorPort = 30001;
constexpr unsigned short kListenerPort = 30003;
// Nothing listens here or on the port after it.
constexpr unsigned short kSilentPort = 30010;

// Hang guard for a pong and for a disconnection notification. On loopback each arrives a
// few update cycles after the send, tens of milliseconds.
constexpr TimeMS kReplyBudgetMs = 5000;

// How long a test watches for a pong that must not come. A ping to loopback is answered
// within a few update cycles.
constexpr TimeMS kNoPongWindowMs = 300;

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

// ID_UDP_PROXY_PING_SERVERS_FROM_COORDINATOR_TO_CLIENT, laid out as UDPProxyCoordinator
// writes it.
void WritePingServers( BitStream& bs, const std::vector<SystemAddress>& servers, RakNetGUID targetGuid = kTargetGuid )
{
    bs.Write( (MessageID)ID_UDP_PROXY_GENERAL );
    bs.Write( (MessageID)ID_UDP_PROXY_PING_SERVERS_FROM_COORDINATOR_TO_CLIENT );
    bs.Write( UNASSIGNED_SYSTEM_ADDRESS );
    bs.Write( kTargetAddress );
    bs.Write( targetGuid );
    bs.Write( (unsigned short)servers.size() );
    for( const SystemAddress& server : servers )
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

    ConnectionWaits::ConnectAndWait( coordinator, client );
    ConnectionWaits::ConnectAndWait( other, client );
    const SystemAddress coordinatorAddress = client->GetSystemAddressFromGuid( coordinator->GetMyGUID() );

    SECTION( "With nothing designated, nothing is acted on" )
    {
        BitStream pingServers;
        WritePingServers( pingServers, { Loopback( kListenerPort ) } );
        MarkerInjection::Inject( coordinator, client, pingServers );
        CHECK( proxyClient.pingServerGroups.empty() );

        BitStream notification;
        WriteResult( notification, ID_UDP_PROXY_FORWARDING_NOTIFICATION, kTargetAddress, Loopback( kClientPort ), kTargetGuid );
        const std::vector<MessageID> received = MarkerInjection::Inject( coordinator, client, notification );
        CHECK( handler.notifications == 0 );
        CHECK( !Contains( received, ID_UNCONNECTED_PONG ) );
        CHECK( !ConnectionWaits::WaitForMessage( client, ID_UNCONNECTED_PONG, kNoPongWindowMs ) );
    }

    SECTION( "A Designated coordinator is acted on, and no one else" )
    {
        proxyClient.AddCoordinator( coordinatorAddress );

        BitStream forgedPingServers;
        WritePingServers( forgedPingServers, { Loopback( kListenerPort ) } );
        MarkerInjection::Inject( other, client, forgedPingServers );
        CHECK( proxyClient.pingServerGroups.empty() );

        BitStream pingServers;
        WritePingServers( pingServers, { Loopback( kListenerPort ) } );
        MarkerInjection::Inject( coordinator, client, pingServers );
        CHECK( proxyClient.pingServerGroups.size() == 1 );

        BitStream forgedNotification;
        WriteResult( forgedNotification, ID_UDP_PROXY_FORWARDING_NOTIFICATION, kTargetAddress, Loopback( kClientPort ), kTargetGuid );
        const std::vector<MessageID> forged = MarkerInjection::Inject( other, client, forgedNotification );
        CHECK( handler.notifications == 0 );
        CHECK( !Contains( forged, ID_UNCONNECTED_PONG ) );
        CHECK( !ConnectionWaits::WaitForMessage( client, ID_UNCONNECTED_PONG, kNoPongWindowMs ) );

        BitStream notification;
        WriteResult( notification, ID_UDP_PROXY_FORWARDING_NOTIFICATION, kTargetAddress, Loopback( kClientPort ), kTargetGuid );
        const std::vector<MessageID> genuine = MarkerInjection::Inject( coordinator, client, notification );
        CHECK( handler.notifications == 1 );
        CHECK( ( Contains( genuine, ID_UNCONNECTED_PONG ) || ConnectionWaits::WaitForMessage( client, ID_UNCONNECTED_PONG, kReplyBudgetMs ) ) );
    }

    SECTION( "RemoveCoordinator withdraws the designation" )
    {
        proxyClient.AddCoordinator( coordinatorAddress );
        proxyClient.RemoveCoordinator( coordinatorAddress );

        BitStream pingServers;
        WritePingServers( pingServers, { Loopback( kListenerPort ) } );
        MarkerInjection::Inject( coordinator, client, pingServers );
        CHECK( proxyClient.pingServerGroups.empty() );

        BitStream notification;
        WriteResult( notification, ID_UDP_PROXY_FORWARDING_NOTIFICATION, kTargetAddress, Loopback( kClientPort ), kTargetGuid );
        MarkerInjection::Inject( coordinator, client, notification );
        CHECK( handler.notifications == 0 );
    }

    SECTION( "A designation lapses when its connection closes" )
    {
        proxyClient.AddCoordinator( coordinatorAddress );

        coordinator->CloseConnection( Loopback( kClientPort ), true );
        ConnectionWaits::WaitForDisconnect( coordinator, Loopback( kClientPort ) );
        REQUIRE( ConnectionWaits::WaitForMessage( client, ID_DISCONNECTION_NOTIFICATION, kReplyBudgetMs ) );

        // The same address again, but not the same designation.
        ConnectionWaits::ConnectAndWait( coordinator, client );
        REQUIRE( client->GetSystemAddressFromGuid( coordinator->GetMyGUID() ) == coordinatorAddress );

        BitStream pingServers;
        WritePingServers( pingServers, { Loopback( kListenerPort ) } );
        MarkerInjection::Inject( coordinator, client, pingServers );
        CHECK( proxyClient.pingServerGroups.empty() );

        BitStream notification;
        WriteResult( notification, ID_UDP_PROXY_FORWARDING_NOTIFICATION, kTargetAddress, Loopback( kClientPort ), kTargetGuid );
        MarkerInjection::Inject( coordinator, client, notification );
        CHECK( handler.notifications == 0 );
    }

    client->DetachPlugin( &proxyClient );
}

TEST_CASE( "UDPProxyClient pings exactly the servers a ping-servers Message lists", "[udpproxy][network]" )
{
    UDPProxyClient proxyClient;

    PeerScope peers;
    RakPeerInterface* client = peers.Server( kClientPort, 4 );
    RakPeerInterface* coordinator = peers.Client( kCoordinatorPort );
    client->AttachPlugin( &proxyClient );

    ConnectionWaits::ConnectAndWait( coordinator, client );
    proxyClient.AddCoordinator( client->GetSystemAddressFromGuid( coordinator->GetMyGUID() ) );

    // Nothing listens on either, so no pong completes the group before it is looked at.
    const std::vector<SystemAddress> servers{ Loopback( kSilentPort ), Loopback( kSilentPort + 1 ) };
    // A reader that skips the GUID takes its top 16 bits as the server count.
    BitStream pingServers;
    WritePingServers( pingServers, servers, RakNetGUID( 0xFFFF000000000001ull ) );
    MarkerInjection::Inject( coordinator, client, pingServers );

    REQUIRE( proxyClient.pingServerGroups.size() == 1 );
    std::vector<SystemAddress> pinged;
    for( const UDPProxyClient::ServerWithPing& server : proxyClient.pingServerGroups.front()->serversToPing )
        pinged.push_back( server.serverAddress );
    CHECK( pinged == servers );

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

    ConnectionWaits::ConnectAndWait( coordinator, client );
    ConnectionWaits::ConnectAndWait( other, client );
    const SystemAddress coordinatorAddress = client->GetSystemAddressFromGuid( coordinator->GetMyGUID() );
    // What the coordinator writes as the source, whatever source was passed.
    const SystemAddress requester = coordinator->GetSystemAddressFromGuid( client->GetMyGUID() );

    SECTION( "A result nothing asked for fires nothing" )
    {
        for( MessageID resultId : { ID_UDP_PROXY_FORWARDING_SUCCEEDED, ID_UDP_PROXY_IN_PROGRESS, ID_UDP_PROXY_ALL_SERVERS_BUSY,
                                    ID_UDP_PROXY_NO_SERVERS_ONLINE, ID_UDP_PROXY_RECIPIENT_GUID_NOT_CONNECTED_TO_COORDINATOR } )
        {
            BitStream result;
            WriteResult( result, resultId, requester, kTargetAddress, kTargetGuid );
            MarkerInjection::Inject( coordinator, client, result );
        }
        CHECK( handler.Total() == 0 );
    }

    SECTION( "A result from a System other than the coordinator asked fires nothing" )
    {
        REQUIRE( proxyClient.RequestForwarding( coordinatorAddress, UNASSIGNED_SYSTEM_ADDRESS, kTargetGuid, kRequestTimeoutMs ) );

        BitStream forged;
        WriteResult( forged, ID_UDP_PROXY_FORWARDING_SUCCEEDED, requester, kTargetAddress, kTargetGuid );
        MarkerInjection::Inject( other, client, forged );
        CHECK( handler.Total() == 0 );

        // The request is still outstanding.
        BitStream genuine;
        WriteResult( genuine, ID_UDP_PROXY_FORWARDING_SUCCEEDED, requester, kTargetAddress, kTargetGuid );
        MarkerInjection::Inject( coordinator, client, genuine );
        CHECK( handler.successes == 1 );
    }

    SECTION( "A result for a different target fires nothing" )
    {
        REQUIRE( proxyClient.RequestForwarding( coordinatorAddress, UNASSIGNED_SYSTEM_ADDRESS, kTargetGuid, kRequestTimeoutMs ) );

        BitStream otherGuid;
        WriteResult( otherGuid, ID_UDP_PROXY_FORWARDING_SUCCEEDED, requester, kTargetAddress, RakNetGUID( 2002 ) );
        MarkerInjection::Inject( coordinator, client, otherGuid );
        CHECK( handler.Total() == 0 );
    }

    SECTION( "A request naming another source takes the result the coordinator writes for the requester" )
    {
        // The coordinator ignores the source passed and writes the requester's own address.
        REQUIRE( proxyClient.RequestForwarding( coordinatorAddress, SystemAddress( "10.0.2.1", 3001 ), kTargetGuid, kRequestTimeoutMs ) );

        BitStream genuine;
        WriteResult( genuine, ID_UDP_PROXY_FORWARDING_SUCCEEDED, requester, kTargetAddress, kTargetGuid );
        MarkerInjection::Inject( coordinator, client, genuine );
        CHECK( handler.successes == 1 );
    }

    SECTION( "A request by address matches on the address" )
    {
        REQUIRE( proxyClient.RequestForwarding( coordinatorAddress, UNASSIGNED_SYSTEM_ADDRESS, kTargetAddress, kRequestTimeoutMs ) );

        BitStream otherAddress;
        WriteResult( otherAddress, ID_UDP_PROXY_FORWARDING_SUCCEEDED, requester, SystemAddress( "10.0.1.3", 2003 ), UNASSIGNED_RAKNET_GUID );
        MarkerInjection::Inject( coordinator, client, otherAddress );
        CHECK( handler.Total() == 0 );

        BitStream genuine;
        WriteResult( genuine, ID_UDP_PROXY_FORWARDING_SUCCEEDED, requester, kTargetAddress, UNASSIGNED_RAKNET_GUID );
        MarkerInjection::Inject( coordinator, client, genuine );
        CHECK( handler.successes == 1 );
    }

    SECTION( "In progress keeps the request, and success retires it" )
    {
        REQUIRE( proxyClient.RequestForwarding( coordinatorAddress, UNASSIGNED_SYSTEM_ADDRESS, kTargetGuid, kRequestTimeoutMs ) );

        for( int i = 0; i < 2; i++ )
        {
            BitStream inProgress;
            WriteResult( inProgress, ID_UDP_PROXY_IN_PROGRESS, requester, kTargetAddress, kTargetGuid );
            MarkerInjection::Inject( coordinator, client, inProgress );
        }
        CHECK( handler.inProgress == 2 );

        for( int i = 0; i < 2; i++ )
        {
            BitStream success;
            WriteResult( success, ID_UDP_PROXY_FORWARDING_SUCCEEDED, requester, kTargetAddress, kTargetGuid );
            MarkerInjection::Inject( coordinator, client, success );
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
                MarkerInjection::Inject( coordinator, client, result );
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
        REQUIRE( ConnectionWaits::WaitForMessage( client, ID_DISCONNECTION_NOTIFICATION, kReplyBudgetMs ) );

        ConnectionWaits::ConnectAndWait( coordinator, client );
        REQUIRE( client->GetSystemAddressFromGuid( coordinator->GetMyGUID() ) == coordinatorAddress );

        BitStream result;
        WriteResult( result, ID_UDP_PROXY_FORWARDING_SUCCEEDED, requester, kTargetAddress, kTargetGuid );
        MarkerInjection::Inject( coordinator, client, result );
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

    ConnectionWaits::ConnectAndWait( coordinator, client );
    const SystemAddress coordinatorAddress = client->GetSystemAddressFromGuid( coordinator->GetMyGUID() );
    const SystemAddress requester = coordinator->GetSystemAddressFromGuid( client->GetMyGUID() );

    SECTION( "A request is forgotten after timeoutOnNoDataMS" )
    {
        constexpr TimeMS kShortTimeoutMs = 50;
        REQUIRE( proxyClient.RequestForwarding( coordinatorAddress, UNASSIGNED_SYSTEM_ADDRESS, kTargetGuid, kShortTimeoutMs ) );
        std::this_thread::sleep_for( std::chrono::milliseconds( kShortTimeoutMs * 4 ) );

        BitStream late;
        WriteResult( late, ID_UDP_PROXY_FORWARDING_SUCCEEDED, requester, kTargetAddress, kTargetGuid );
        MarkerInjection::Inject( coordinator, client, late );
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
        MarkerInjection::Inject( coordinator, client, final );
        CHECK( handler.noServersOnline == 1 );
        CHECK( proxyClient.RequestForwarding( coordinatorAddress, UNASSIGNED_SYSTEM_ADDRESS, kTargetGuid, kRequestTimeoutMs ) );
    }

    client->DetachPlugin( &proxyClient );
}
