#include "Plugins/Router2.h"

#include "BitStream.h"
#include "ConnectionWaits.h"
#include "GetTime.h"
#include "MessageIdentifiers.h"
#include "PeerScope.h"
#include "RakNetStringMakers.h"
#include "RakNetTypes.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <functional>
#include <thread>
#include <vector>

/*
Router2's own flow, end to end on loopback: a source reaches an endpoint through a router,
and when that router goes, through another.

The first needs nothing designated. The second needs the endpoint to have designated both
routers, since only a Designated router may move a live connection, and the endpoint checks
the move against what the first router announced.
*/

using namespace RakNet;

namespace {

// Ports no other test uses.
constexpr unsigned short kRouterPort = 31000;
constexpr unsigned short kSecondRouterPort = 31001;
constexpr unsigned short kSourcePort = 31002;
constexpr unsigned short kEndpointPort = 31003;
constexpr unsigned short kStrayPort = 31004;
constexpr unsigned short kSecondEndpointPort = 31005;

// Hang guard for each step. On loopback each takes a few update cycles, tens of
// milliseconds, apart from the query step, which waits for every router to answer.
constexpr TimeMS kStepBudgetMs = 10000;

constexpr MessageID kUserMessage = ID_USER_PACKET_ENUM;

struct Received
{
    RakPeerInterface* peer;
    MessageID id;
    RakNetGUID guid;
    std::vector<unsigned char> data;
};

// Receives on every peer, since Router2 runs inside Receive, until one Message satisfies
// wanted. Returns it, or a Received with a null peer once budgetMs has passed.
Received PumpUntil( const std::vector<RakPeerInterface*>& peers, const std::function<bool( const Received& )>& wanted, TimeMS budgetMs = kStepBudgetMs )
{
    const TimeMS deadline = GetTimeMS() + budgetMs;
    while( !ConnectionWaits::Expired( deadline ) )
    {
        for( RakPeerInterface* peer : peers )
        {
            for( Packet* packet = peer->Receive(); packet != nullptr; packet = peer->Receive() )
            {
                Received each{ peer, packet->data[0], packet->guid, std::vector<unsigned char>( packet->data, packet->data + packet->length ) };
                peer->DeallocatePacket( packet );
                if( wanted( each ) )
                    return each;
            }
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
    }
    return Received{ nullptr, 0, UNASSIGNED_RAKNET_GUID, {} };
}

Received PumpUntil( const std::vector<RakPeerInterface*>& peers, RakPeerInterface* peer, MessageID id )
{
    return PumpUntil( peers, [peer, id]( const Received& each ) { return each.peer == peer && each.id == id; } );
}

// Returns once both ends hold the connection. The client is accepted before the server
// finishes its side, and an Intermediary turns down a query for an Endpoint it has no ping
// to yet.
void Connect( const std::vector<RakPeerInterface*>& peers, RakPeerInterface* client, unsigned short port )
{
    REQUIRE( client->Connect( "127.0.0.1", port, nullptr, 0 ) == CONNECTION_ATTEMPT_STARTED );
    bool accepted = false;
    bool incoming = false;
    const Received last = PumpUntil( peers, [&]( const Received& each ) {
        if( each.peer == client && each.id == ID_CONNECTION_REQUEST_ACCEPTED )
            accepted = true;
        if( each.peer != client && each.id == ID_NEW_INCOMING_CONNECTION && each.guid == client->GetMyGUID() )
            incoming = true;
        return accepted && incoming;
    } );
    REQUIRE( last.peer != nullptr );
}

// Sends a user Message from one end to the other by RakNetGUID, and checks it arrives.
void CheckReaches( const std::vector<RakPeerInterface*>& peers, RakPeerInterface* from, RakPeerInterface* to )
{
    BitStream bs;
    bs.Write( kUserMessage );
    REQUIRE( from->Send( &bs, HIGH_PRIORITY, RELIABLE_ORDERED, 0, to->GetMyGUID(), false ) != 0 );
    const Received received = PumpUntil( peers, to, kUserMessage );
    CHECK( received.peer == to );
    CHECK( received.guid == from->GetMyGUID() );
}

// How Router2 tells the source it could not route to endpoint: ID_ROUTER_2_FORWARDING_NO_PATH
// for a new connection, ID_CONNECTION_LOST for a live one.
bool IsNoRoute( const Received& each, RakPeerInterface* source, RakPeerInterface* endpoint )
{
    return each.peer == source && each.guid == endpoint->GetMyGUID() &&
           ( each.id == ID_ROUTER_2_FORWARDING_NO_PATH || each.id == ID_CONNECTION_LOST );
}

void FailNoRoute( const Received& each )
{
    FAIL( "the source got " << ( each.id == ID_ROUTER_2_FORWARDING_NO_PATH ? "ID_ROUTER_2_FORWARDING_NO_PATH" : "ID_CONNECTION_LOST" ) << " for the endpoint" );
}

// After EstablishRouting, waits for the route and connects through the router it reports.
// With endpointRerouted, the wait also takes the endpoint's ID_ROUTER_2_REROUTED for the
// source, which the router sends only after a completed punch and the endpoint passes on
// only from a Designated router. Returns the router's RakNetGUID.
RakNetGUID ConnectThroughRoute( const std::vector<RakPeerInterface*>& peers, RakPeerInterface* source, RakPeerInterface* endpoint, bool endpointRerouted = false )
{
    Received established{ nullptr, 0, UNASSIGNED_RAKNET_GUID, {} };
    bool sawRerouted = false;
    const Received last = PumpUntil( peers, [&]( const Received& each ) {
        if( each.peer == endpoint && each.id == ID_ROUTER_2_REROUTED )
            sawRerouted = true;
        if( ( each.peer == source && each.id == ID_ROUTER_2_FORWARDING_ESTABLISHED ) || IsNoRoute( each, source, endpoint ) )
            established = each;
        return established.peer != nullptr && ( sawRerouted || !endpointRerouted || established.id != ID_ROUTER_2_FORWARDING_ESTABLISHED );
    } );
    REQUIRE( last.peer != nullptr );
    REQUIRE( established.peer == source );
    if( established.id != ID_ROUTER_2_FORWARDING_ESTABLISHED )
        FailNoRoute( established );

    std::vector<unsigned char> data = established.data;
    BitStream bs( data.data(), (unsigned int)data.size(), false );
    bs.IgnoreBytes( sizeof( MessageID ) );
    RakNetGUID endpointGuid;
    unsigned short forwardingPort = 0;
    REQUIRE( bs.Read( endpointGuid ) );
    REQUIRE( bs.Read( forwardingPort ) );
    CHECK( endpointGuid == endpoint->GetMyGUID() );
    REQUIRE( forwardingPort != 0 );

    REQUIRE( source->Connect( "127.0.0.1", forwardingPort, nullptr, 0 ) == CONNECTION_ATTEMPT_STARTED );
    const Received accepted = PumpUntil( peers, [source, endpoint]( const Received& each ) {
        return each.peer == source && each.id == ID_CONNECTION_REQUEST_ACCEPTED && each.guid == endpoint->GetMyGUID();
    } );
    REQUIRE( accepted.peer == source );
    CHECK( source->GetSystemAddressFromGuid( endpoint->GetMyGUID() ) == SystemAddress( "127.0.0.1", forwardingPort ) );
    return established.guid;
}

// Runs EstablishRouting from source to endpoint and connects through the router it
// reports. Returns the router's RakNetGUID.
RakNetGUID Route( const std::vector<RakPeerInterface*>& peers, Router2& sourceRouter, RakPeerInterface* source, RakPeerInterface* endpoint, bool endpointRerouted = false )
{
    sourceRouter.EstablishRouting( endpoint->GetMyGUID() );
    return ConnectThroughRoute( peers, source, endpoint, endpointRerouted );
}

// Router2's internal sub-identifiers for a forwarding query, its reply, and a forwarding request.
constexpr unsigned char kQueryForwarding = 0;
constexpr unsigned char kReplyForwarding = 1;
constexpr unsigned char kRequestForwarding = 2;

// Sends router a forwarding request for endpoint from source, as source's Router2 does once
// it has picked router.
void RequestForwarding( RakPeerInterface* source, RakPeerInterface* router, RakPeerInterface* endpoint )
{
    BitStream request;
    request.Write( (MessageID)ID_ROUTER_2_INTERNAL );
    request.Write( kRequestForwarding );
    request.Write( endpoint->GetMyGUID() );
    REQUIRE( source->Send( &request, MEDIUM_PRIORITY, RELIABLE_ORDERED, 0, router->GetMyGUID(), false ) != 0 );
}

struct ForwardingReply
{
    bool canForward = false;
    unsigned short forwardedPairs = 0;
};

// Sends router a forwarding query for endpoint from prober, a peer with no Router2 attached,
// and reads the reply.
ForwardingReply QueryForwarding( const std::vector<RakPeerInterface*>& peers, RakPeerInterface* prober, RakPeerInterface* router, RakPeerInterface* endpoint )
{
    BitStream query;
    query.Write( (MessageID)ID_ROUTER_2_INTERNAL );
    query.Write( kQueryForwarding );
    query.Write( endpoint->GetMyGUID() );
    REQUIRE( prober->Send( &query, MEDIUM_PRIORITY, RELIABLE_ORDERED, 0, router->GetMyGUID(), false ) != 0 );

    const Received received = PumpUntil( peers, [prober]( const Received& each ) {
        return each.peer == prober && each.id == ID_ROUTER_2_INTERNAL && each.data.size() > 1 && each.data[1] == kReplyForwarding;
    } );
    REQUIRE( received.peer == prober );

    std::vector<unsigned char> data = received.data;
    BitStream bs( data.data(), (unsigned int)data.size(), false );
    bs.IgnoreBytes( sizeof( MessageID ) + sizeof( unsigned char ) );
    RakNetGUID endpointGuid;
    ForwardingReply reply;
    REQUIRE( bs.Read( endpointGuid ) );
    CHECK( endpointGuid == endpoint->GetMyGUID() );
    REQUIRE( bs.Read( reply.canForward ) );
    if( reply.canForward )
    {
        unsigned short pingToEndpoint = 0;
        REQUIRE( bs.Read( pingToEndpoint ) );
        REQUIRE( bs.Read( reply.forwardedPairs ) );
    }
    return reply;
}

// Whether EstablishRouting from peer would treat other as not connected.
bool IsClosedTo( RakPeerInterface* peer, RakPeerInterface* other )
{
    const ConnectionState state = peer->GetConnectionState( other->GetMyGUID() );
    return state == IS_DISCONNECTED || state == IS_NOT_CONNECTED;
}

// Closes the connection between a and b on both ends without notifying either, and waits
// until both see it closed.
void CloseSilently( const std::vector<RakPeerInterface*>& peers, RakPeerInterface* a, RakPeerInterface* b )
{
    a->CloseConnection( b->GetMyGUID(), false );
    b->CloseConnection( a->GetMyGUID(), false );
    const TimeMS deadline = GetTimeMS() + kStepBudgetMs;
    while( !( IsClosedTo( a, b ) && IsClosedTo( b, a ) ) && !ConnectionWaits::Expired( deadline ) )
        PumpUntil( peers, []( const Received& ) { return false; }, 10 );
    REQUIRE( IsClosedTo( a, b ) );
    REQUIRE( IsClosedTo( b, a ) );
}

} // namespace

TEST_CASE( "Router2 forwards a new connection with nothing designated", "[router2][network]" )
{
    // Before the PeerScope, so they outlive the peers.
    Router2 routerPlugin, sourcePlugin, endpointPlugin;
    routerPlugin.SetMaximumForwardingRequests( 4 );

    PeerScope scope;
    RakPeerInterface* router = scope.Server( kRouterPort, 4 );
    RakPeerInterface* source = scope.Server( kSourcePort, 4 );
    RakPeerInterface* endpoint = scope.Server( kEndpointPort, 4 );
    router->AttachPlugin( &routerPlugin );
    source->AttachPlugin( &sourcePlugin );
    endpoint->AttachPlugin( &endpointPlugin );
    const std::vector<RakPeerInterface*> peers{ router, source, endpoint };

    Connect( peers, source, kRouterPort );
    Connect( peers, endpoint, kRouterPort );

    // Outside the CHECK, so a failed REQUIRE inside Route ends the test case.
    const RakNetGUID used = Route( peers, sourcePlugin, source, endpoint );
    CHECK( used == router->GetMyGUID() );
    CheckReaches( peers, source, endpoint );
    CheckReaches( peers, endpoint, source );

    router->DetachPlugin( &routerPlugin );
    source->DetachPlugin( &sourcePlugin );
    endpoint->DetachPlugin( &endpointPlugin );
}

TEST_CASE( "Router2 tells the source when no router can reach the endpoint", "[router2][network]" )
{
    Router2 routerPlugin, sourcePlugin, endpointPlugin;
    routerPlugin.SetMaximumForwardingRequests( 4 );

    PeerScope scope;
    RakPeerInterface* router = scope.Server( kRouterPort, 4 );
    RakPeerInterface* source = scope.Server( kSourcePort, 4 );
    RakPeerInterface* endpoint = scope.Server( kEndpointPort, 4 );
    router->AttachPlugin( &routerPlugin );
    source->AttachPlugin( &sourcePlugin );
    endpoint->AttachPlugin( &endpointPlugin );
    const std::vector<RakPeerInterface*> peers{ router, source, endpoint };

    Connect( peers, source, kRouterPort );
    Connect( peers, endpoint, kRouterPort );

    // The endpoint answers the router's punch only inside Receive, so never pumping it means
    // the punch never completes.
    sourcePlugin.EstablishRouting( endpoint->GetMyGUID() );
    const Received ended = PumpUntil( { router, source }, [source, endpoint]( const Received& each ) {
        return ( each.peer == source && each.id == ID_ROUTER_2_FORWARDING_ESTABLISHED ) || IsNoRoute( each, source, endpoint );
    } );
    REQUIRE( ended.peer == source );
    CHECK( ended.id == ID_ROUTER_2_FORWARDING_NO_PATH );
    CHECK( ended.guid == endpoint->GetMyGUID() );

    router->DetachPlugin( &routerPlugin );
    source->DetachPlugin( &sourcePlugin );
    endpoint->DetachPlugin( &endpointPlugin );
}

// Finding no route, and restarting a forwarded connection whose router went, both leave the
// source able to route again.
TEST_CASE( "Router2 routes after finding no route and loses the route with its router", "[router2][network]" )
{
    Router2 routerPlugin, sourcePlugin, endpointPlugin;
    routerPlugin.SetMaximumForwardingRequests( 4 );

    PeerScope scope;
    RakPeerInterface* router = scope.Server( kRouterPort, 4 );
    RakPeerInterface* source = scope.Server( kSourcePort, 4 );
    RakPeerInterface* endpoint = scope.Server( kEndpointPort, 4 );
    router->AttachPlugin( &routerPlugin );
    source->AttachPlugin( &sourcePlugin );
    endpoint->AttachPlugin( &endpointPlugin );
    std::vector<RakPeerInterface*> peers{ router, source, endpoint };

    // The router is not yet connected to the endpoint, so it answers the query with no route.
    Connect( peers, source, kRouterPort );
    sourcePlugin.EstablishRouting( endpoint->GetMyGUID() );
    const Received noPath = PumpUntil( peers, [source, endpoint]( const Received& each ) {
        return ( each.peer == source && each.id == ID_ROUTER_2_FORWARDING_ESTABLISHED ) || IsNoRoute( each, source, endpoint );
    } );
    REQUIRE( noPath.peer == source );
    REQUIRE( noPath.id == ID_ROUTER_2_FORWARDING_NO_PATH );

    Connect( peers, endpoint, kRouterPort );
    const RakNetGUID used = Route( peers, sourcePlugin, source, endpoint );
    REQUIRE( used == router->GetMyGUID() );

    // With the router gone, the source has no one else to ask.
    router->Shutdown( 100 );
    peers = { source, endpoint };
    const Received lost = PumpUntil( peers, [source, endpoint]( const Received& each ) {
        return ( each.peer == source && each.id == ID_ROUTER_2_REROUTED ) || IsNoRoute( each, source, endpoint );
    } );
    REQUIRE( lost.peer == source );
    CHECK( lost.id == ID_CONNECTION_LOST );

    router->DetachPlugin( &routerPlugin );
    source->DetachPlugin( &sourcePlugin );
    endpoint->DetachPlugin( &endpointPlugin );
}

TEST_CASE( "Router2 re-routes a live forwarded connection through a Designated router", "[router2][network]" )
{
    Router2 firstPlugin, secondPlugin, sourcePlugin, endpointPlugin;
    firstPlugin.SetMaximumForwardingRequests( 4 );
    secondPlugin.SetMaximumForwardingRequests( 4 );

    PeerScope scope;
    RakPeerInterface* first = scope.Server( kRouterPort, 4 );
    RakPeerInterface* second = scope.Server( kSecondRouterPort, 4 );
    RakPeerInterface* source = scope.Server( kSourcePort, 4 );
    RakPeerInterface* endpoint = scope.Server( kEndpointPort, 4 );
    first->AttachPlugin( &firstPlugin );
    second->AttachPlugin( &secondPlugin );
    source->AttachPlugin( &sourcePlugin );
    endpoint->AttachPlugin( &endpointPlugin );
    std::vector<RakPeerInterface*> peers{ first, second, source, endpoint };

    Connect( peers, source, kRouterPort );
    Connect( peers, endpoint, kRouterPort );
    Connect( peers, source, kSecondRouterPort );
    Connect( peers, endpoint, kSecondRouterPort );
    endpointPlugin.AddIntermediary( SystemAddress( "127.0.0.1", kRouterPort ) );
    endpointPlugin.AddIntermediary( SystemAddress( "127.0.0.1", kSecondRouterPort ) );

    // The source picks whichever router answers with the lower ping.
    const RakNetGUID used = Route( peers, sourcePlugin, source, endpoint );
    REQUIRE( ( used == first->GetMyGUID() || used == second->GetMyGUID() ) );
    RakPeerInterface* lost = used == first->GetMyGUID() ? first : second;
    RakPeerInterface* remaining = lost == first ? second : first;
    CheckReaches( peers, source, endpoint );
    const SystemAddress sourceSeesEndpointAt = source->GetSystemAddressFromGuid( endpoint->GetMyGUID() );
    const SystemAddress endpointSeesSourceAt = endpoint->GetSystemAddressFromGuid( source->GetMyGUID() );

    lost->Shutdown( 100 );
    peers = { remaining, source, endpoint };

    // The source moves on its own forwarding success and reports it as ID_ROUTER_2_REROUTED,
    // and so does the endpoint, on the router's word.
    bool sourceRerouted = false;
    bool endpointRerouted = false;
    const Received last = PumpUntil( peers, [&]( const Received& each ) {
        if( each.id == ID_ROUTER_2_REROUTED && each.peer == source )
            sourceRerouted = true;
        if( each.id == ID_ROUTER_2_REROUTED && each.peer == endpoint )
            endpointRerouted = true;
        return ( sourceRerouted && endpointRerouted ) || IsNoRoute( each, source, endpoint );
    } );
    REQUIRE( last.peer != nullptr );
    if( IsNoRoute( last, source, endpoint ) )
        FailNoRoute( last );

    // ChangeSystemAddress only queues a command, so the move shows in the getters a cycle later.
    const TimeMS deadline = GetTimeMS() + kStepBudgetMs;
    while( ( source->GetSystemAddressFromGuid( endpoint->GetMyGUID() ) == sourceSeesEndpointAt ||
             endpoint->GetSystemAddressFromGuid( source->GetMyGUID() ) == endpointSeesSourceAt ) &&
           !ConnectionWaits::Expired( deadline ) )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
    }
    CHECK( source->GetSystemAddressFromGuid( endpoint->GetMyGUID() ) != sourceSeesEndpointAt );
    CHECK( endpoint->GetSystemAddressFromGuid( source->GetMyGUID() ) != endpointSeesSourceAt );

    CheckReaches( peers, source, endpoint );
    CheckReaches( peers, endpoint, source );

    first->DetachPlugin( &firstPlugin );
    second->DetachPlugin( &secondPlugin );
    source->DetachPlugin( &sourcePlugin );
    endpoint->DetachPlugin( &endpointPlugin );
}

TEST_CASE( "Router2 forwards a connection whose endpoint answers the punch late", "[router2][network]" )
{
    // Longer than a loopback ping, short of what a busy host can stall a Peer for.
    constexpr TimeMS kPunchStallMs = 1000;

    Router2 routerPlugin, sourcePlugin, endpointPlugin;
    routerPlugin.SetMaximumForwardingRequests( 4 );

    PeerScope scope;
    RakPeerInterface* router = scope.Server( kRouterPort, 4 );
    RakPeerInterface* source = scope.Server( kSourcePort, 4 );
    RakPeerInterface* endpoint = scope.Server( kEndpointPort, 4 );
    router->AttachPlugin( &routerPlugin );
    source->AttachPlugin( &sourcePlugin );
    endpoint->AttachPlugin( &endpointPlugin );
    const std::vector<RakPeerInterface*> peers{ router, source, endpoint };

    Connect( peers, source, kRouterPort );
    Connect( peers, endpoint, kRouterPort );

    // The endpoint answers the router's punch only inside Receive, so not pumping it holds
    // the punch back.
    sourcePlugin.EstablishRouting( endpoint->GetMyGUID() );
    const Received early = PumpUntil( { router, source }, [source, endpoint]( const Received& each ) { return IsNoRoute( each, source, endpoint ); }, kPunchStallMs );
    if( early.peer != nullptr )
        FailNoRoute( early );

    const RakNetGUID used = ConnectThroughRoute( peers, source, endpoint );
    CHECK( used == router->GetMyGUID() );
    CheckReaches( peers, source, endpoint );
    CheckReaches( peers, endpoint, source );

    router->DetachPlugin( &routerPlugin );
    source->DetachPlugin( &sourcePlugin );
    endpoint->DetachPlugin( &endpointPlugin );
}

TEST_CASE( "Router2 tells a source connected to nobody that there is no route", "[router2][network]" )
{
    Router2 sourcePlugin;

    PeerScope scope;
    RakPeerInterface* source = scope.Server( kSourcePort, 4 );
    RakPeerInterface* endpoint = scope.Server( kEndpointPort, 4 );
    source->AttachPlugin( &sourcePlugin );

    sourcePlugin.EstablishRouting( endpoint->GetMyGUID() );
    const Received ended = PumpUntil( { source }, [source, endpoint]( const Received& each ) {
        return ( each.peer == source && each.id == ID_ROUTER_2_FORWARDING_ESTABLISHED ) || IsNoRoute( each, source, endpoint );
    } );
    REQUIRE( ended.peer == source );
    CHECK( ended.id == ID_ROUTER_2_FORWARDING_NO_PATH );

    source->DetachPlugin( &sourcePlugin );
}

TEST_CASE( "Router2 tells the source a forwarded connection is lost when no Intermediary is left", "[router2][network]" )
{
    Router2 routerPlugin, sourcePlugin, endpointPlugin;
    routerPlugin.SetMaximumForwardingRequests( 4 );

    PeerScope scope;
    RakPeerInterface* router = scope.Server( kRouterPort, 4 );
    RakPeerInterface* source = scope.Server( kSourcePort, 4 );
    RakPeerInterface* endpoint = scope.Server( kEndpointPort, 4 );
    router->AttachPlugin( &routerPlugin );
    source->AttachPlugin( &sourcePlugin );
    endpoint->AttachPlugin( &endpointPlugin );
    std::vector<RakPeerInterface*> peers{ router, source, endpoint };

    Connect( peers, source, kRouterPort );
    Connect( peers, endpoint, kRouterPort );
    const RakNetGUID used = Route( peers, sourcePlugin, source, endpoint );
    REQUIRE( used == router->GetMyGUID() );

    // The source is left connected to the endpoint alone, so the re-route has no one to ask.
    // The forwarded connection itself times out only after the step budget, so any
    // ID_CONNECTION_LOST within it is Router2's.
    router->Shutdown( 100 );
    peers = { source, endpoint };
    const Received ended = PumpUntil( peers, [source, endpoint]( const Received& each ) {
        return ( each.peer == source && each.id == ID_ROUTER_2_REROUTED ) || IsNoRoute( each, source, endpoint );
    } );
    REQUIRE( ended.peer == source );
    CHECK( ended.id == ID_CONNECTION_LOST );

    router->DetachPlugin( &routerPlugin );
    source->DetachPlugin( &sourcePlugin );
    endpoint->DetachPlugin( &endpointPlugin );
}

TEST_CASE( "Router2 routes again to an endpoint whose re-route found no Intermediary", "[router2][network]" )
{
    Router2 firstPlugin, secondPlugin, sourcePlugin, endpointPlugin;
    firstPlugin.SetMaximumForwardingRequests( 4 );
    secondPlugin.SetMaximumForwardingRequests( 4 );

    PeerScope scope;
    RakPeerInterface* first = scope.Server( kRouterPort, 4 );
    RakPeerInterface* second = scope.Server( kSecondRouterPort, 4 );
    RakPeerInterface* source = scope.Server( kSourcePort, 4 );
    RakPeerInterface* endpoint = scope.Server( kEndpointPort, 4 );
    first->AttachPlugin( &firstPlugin );
    second->AttachPlugin( &secondPlugin );
    source->AttachPlugin( &sourcePlugin );
    endpoint->AttachPlugin( &endpointPlugin );
    std::vector<RakPeerInterface*> peers{ first, source, endpoint };

    Connect( peers, source, kRouterPort );
    Connect( peers, endpoint, kRouterPort );
    REQUIRE( Route( peers, sourcePlugin, source, endpoint ) == first->GetMyGUID() );

    // The re-route this starts has no one to ask.
    first->Shutdown( 100 );
    peers = { second, source, endpoint };
    REQUIRE( PumpUntil( peers, source, ID_DISCONNECTION_NOTIFICATION ).peer == source );
    CloseSilently( peers, source, endpoint );

    Connect( peers, source, kSecondRouterPort );
    Connect( peers, endpoint, kSecondRouterPort );
    const RakNetGUID used = Route( peers, sourcePlugin, source, endpoint );
    CHECK( used == second->GetMyGUID() );
    CheckReaches( peers, source, endpoint );
    CheckReaches( peers, endpoint, source );

    first->DetachPlugin( &firstPlugin );
    second->DetachPlugin( &secondPlugin );
    source->DetachPlugin( &sourcePlugin );
    endpoint->DetachPlugin( &endpointPlugin );
}

// The prober asks about a second endpoint, so its pair is not the one forwarded.
TEST_CASE( "Router2 forwards no more pairs than its forwarding cap", "[router2][network]" )
{
    Router2 routerPlugin, sourcePlugin, endpointPlugin;
    routerPlugin.SetMaximumForwardingRequests( 1 );

    PeerScope scope;
    RakPeerInterface* router = scope.Server( kRouterPort, 4 );
    RakPeerInterface* source = scope.Server( kSourcePort, 4 );
    RakPeerInterface* endpoint = scope.Server( kEndpointPort, 4 );
    RakPeerInterface* secondEndpoint = scope.Server( kSecondEndpointPort, 4 );
    RakPeerInterface* prober = scope.Server( kStrayPort, 4 );
    router->AttachPlugin( &routerPlugin );
    source->AttachPlugin( &sourcePlugin );
    endpoint->AttachPlugin( &endpointPlugin );
    const std::vector<RakPeerInterface*> peers{ router, source, endpoint, secondEndpoint, prober };

    Connect( peers, source, kRouterPort );
    Connect( peers, endpoint, kRouterPort );
    Connect( peers, secondEndpoint, kRouterPort );
    Connect( peers, prober, kRouterPort );

    CHECK( QueryForwarding( peers, prober, router, secondEndpoint ).canForward );
    REQUIRE( Route( peers, sourcePlugin, source, endpoint ) == router->GetMyGUID() );
    CHECK_FALSE( QueryForwarding( peers, prober, router, secondEndpoint ).canForward );

    routerPlugin.SetMaximumForwardingRequests( 2 );
    const ForwardingReply reply = QueryForwarding( peers, prober, router, secondEndpoint );
    CHECK( reply.canForward );
    CHECK( reply.forwardedPairs == 1 );

    router->DetachPlugin( &routerPlugin );
    source->DetachPlugin( &sourcePlugin );
    endpoint->DetachPlugin( &endpointPlugin );
}

TEST_CASE( "Router2 routes again after the router's punch to the endpoint timed out", "[router2][network]" )
{
    Router2 routerPlugin, sourcePlugin, endpointPlugin;
    routerPlugin.SetMaximumForwardingRequests( 4 );

    PeerScope scope;
    RakPeerInterface* router = scope.Server( kRouterPort, 4 );
    RakPeerInterface* source = scope.Server( kSourcePort, 4 );
    RakPeerInterface* endpoint = scope.Server( kEndpointPort, 4 );
    router->AttachPlugin( &routerPlugin );
    source->AttachPlugin( &sourcePlugin );
    endpoint->AttachPlugin( &endpointPlugin );
    const std::vector<RakPeerInterface*> peers{ router, source, endpoint };

    Connect( peers, source, kRouterPort );
    Connect( peers, endpoint, kRouterPort );
    endpointPlugin.AddIntermediary( SystemAddress( "127.0.0.1", kRouterPort ) );

    // The endpoint answers the router's punch only inside Receive, so never pumping it makes
    // the punch time out.
    sourcePlugin.EstablishRouting( endpoint->GetMyGUID() );
    const Received ended = PumpUntil( { router, source }, [source, endpoint]( const Received& each ) {
        return ( each.peer == source && each.id == ID_ROUTER_2_FORWARDING_ESTABLISHED ) || IsNoRoute( each, source, endpoint );
    } );
    REQUIRE( ended.peer == source );
    REQUIRE( ended.id == ID_ROUTER_2_FORWARDING_NO_PATH );

    // The retry punches again, so the endpoint hears of the route as on a first request.
    const RakNetGUID used = Route( peers, sourcePlugin, source, endpoint, true );
    CHECK( used == router->GetMyGUID() );
    CheckReaches( peers, source, endpoint );
    CheckReaches( peers, endpoint, source );

    router->DetachPlugin( &routerPlugin );
    source->DetachPlugin( &sourcePlugin );
    endpoint->DetachPlugin( &endpointPlugin );
}

// The cap counts forwarded pairs, so a pair left behind by the timed-out punch would refuse
// the second.
TEST_CASE( "Router2 frees the forwarded pair of a punch that timed out", "[router2][network]" )
{
    Router2 routerPlugin, sourcePlugin, endpointPlugin, secondEndpointPlugin;
    routerPlugin.SetMaximumForwardingRequests( 1 );

    PeerScope scope;
    RakPeerInterface* router = scope.Server( kRouterPort, 4 );
    RakPeerInterface* source = scope.Server( kSourcePort, 4 );
    RakPeerInterface* endpoint = scope.Server( kEndpointPort, 4 );
    RakPeerInterface* secondEndpoint = scope.Server( kSecondEndpointPort, 4 );
    router->AttachPlugin( &routerPlugin );
    source->AttachPlugin( &sourcePlugin );
    endpoint->AttachPlugin( &endpointPlugin );
    secondEndpoint->AttachPlugin( &secondEndpointPlugin );
    const std::vector<RakPeerInterface*> peers{ router, source, endpoint, secondEndpoint };

    Connect( peers, source, kRouterPort );
    Connect( peers, endpoint, kRouterPort );
    Connect( peers, secondEndpoint, kRouterPort );

    // Never pumping the endpoint makes the punch time out.
    sourcePlugin.EstablishRouting( endpoint->GetMyGUID() );
    const Received ended = PumpUntil( { router, source, secondEndpoint }, [source, endpoint]( const Received& each ) {
        return ( each.peer == source && each.id == ID_ROUTER_2_FORWARDING_ESTABLISHED ) || IsNoRoute( each, source, endpoint );
    } );
    REQUIRE( ended.peer == source );
    REQUIRE( ended.id == ID_ROUTER_2_FORWARDING_NO_PATH );

    REQUIRE( Route( peers, sourcePlugin, source, secondEndpoint ) == router->GetMyGUID() );
    CheckReaches( peers, source, secondEndpoint );

    router->DetachPlugin( &routerPlugin );
    source->DetachPlugin( &sourcePlugin );
    endpoint->DetachPlugin( &endpointPlugin );
    secondEndpoint->DetachPlugin( &secondEndpointPlugin );
}

TEST_CASE( "Router2 frees the forwarded pair of a punch whose endpoint disconnected", "[router2][network]" )
{
    // Long enough for the request to reach the router, well short of the punch timeout.
    constexpr TimeMS kPunchStallMs = 1000;

    Router2 routerPlugin, sourcePlugin, endpointPlugin, secondEndpointPlugin;
    routerPlugin.SetMaximumForwardingRequests( 1 );

    PeerScope scope;
    RakPeerInterface* router = scope.Server( kRouterPort, 4 );
    RakPeerInterface* source = scope.Server( kSourcePort, 4 );
    RakPeerInterface* endpoint = scope.Server( kEndpointPort, 4 );
    RakPeerInterface* secondEndpoint = scope.Server( kSecondEndpointPort, 4 );
    router->AttachPlugin( &routerPlugin );
    source->AttachPlugin( &sourcePlugin );
    endpoint->AttachPlugin( &endpointPlugin );
    secondEndpoint->AttachPlugin( &secondEndpointPlugin );
    const std::vector<RakPeerInterface*> peers{ router, source, endpoint, secondEndpoint };

    Connect( peers, source, kRouterPort );
    Connect( peers, endpoint, kRouterPort );
    Connect( peers, secondEndpoint, kRouterPort );

    // Not pumping the endpoint holds the punch back until it disconnects.
    sourcePlugin.EstablishRouting( endpoint->GetMyGUID() );
    const Received early = PumpUntil( { router, source, secondEndpoint }, [source, endpoint]( const Received& each ) { return IsNoRoute( each, source, endpoint ); }, kPunchStallMs );
    if( early.peer != nullptr )
        FailNoRoute( early );

    endpoint->CloseConnection( router->GetMyGUID(), true );
    const Received ended = PumpUntil( { router, source, secondEndpoint }, [source, endpoint]( const Received& each ) {
        return ( each.peer == source && each.id == ID_ROUTER_2_FORWARDING_ESTABLISHED ) || IsNoRoute( each, source, endpoint );
    } );
    REQUIRE( ended.peer == source );
    REQUIRE( ended.id == ID_ROUTER_2_FORWARDING_NO_PATH );

    REQUIRE( Route( { router, source, secondEndpoint }, sourcePlugin, source, secondEndpoint ) == router->GetMyGUID() );
    CheckReaches( { router, source, secondEndpoint }, source, secondEndpoint );

    router->DetachPlugin( &routerPlugin );
    source->DetachPlugin( &sourcePlugin );
    endpoint->DetachPlugin( &endpointPlugin );
    secondEndpoint->DetachPlugin( &secondEndpointPlugin );
}

// A reply from a system the request never asked is dropped, and the request goes on to
// route through the system it did ask.
TEST_CASE( "Router2 routes after a forwarding reply from a system it did not ask", "[router2][network]" )
{
    Router2 routerPlugin, sourcePlugin, endpointPlugin, secondEndpointPlugin;
    routerPlugin.SetMaximumForwardingRequests( 4 );

    PeerScope scope;
    RakPeerInterface* router = scope.Server( kRouterPort, 4 );
    RakPeerInterface* source = scope.Server( kSourcePort, 4 );
    RakPeerInterface* endpoint = scope.Server( kEndpointPort, 4 );
    RakPeerInterface* secondEndpoint = scope.Server( kSecondEndpointPort, 4 );
    RakPeerInterface* stray = scope.Server( kStrayPort, 4 );
    router->AttachPlugin( &routerPlugin );
    source->AttachPlugin( &sourcePlugin );
    endpoint->AttachPlugin( &endpointPlugin );
    secondEndpoint->AttachPlugin( &secondEndpointPlugin );
    const std::vector<RakPeerInterface*> peers{ router, source, endpoint, secondEndpoint, stray };

    Connect( peers, source, kRouterPort );
    Connect( peers, endpoint, kRouterPort );
    Connect( peers, secondEndpoint, kRouterPort );

    // The router answers the query only inside Receive, so not pumping it keeps the request
    // pending. The stray connects after the query went out, so the request never asked it.
    sourcePlugin.EstablishRouting( endpoint->GetMyGUID() );
    Connect( { source, stray }, stray, kSourcePort );

    BitStream reply;
    reply.Write( (MessageID)ID_ROUTER_2_INTERNAL );
    reply.Write( kReplyForwarding );
    reply.Write( endpoint->GetMyGUID() );
    reply.Write( false );
    REQUIRE( stray->Send( &reply, MEDIUM_PRIORITY, RELIABLE_ORDERED, 0, source->GetMyGUID(), false ) != 0 );
    // Ordered behind the reply, so the source has handled the reply once this arrives.
    BitStream marker;
    marker.Write( kUserMessage );
    REQUIRE( stray->Send( &marker, MEDIUM_PRIORITY, RELIABLE_ORDERED, 0, source->GetMyGUID(), false ) != 0 );
    REQUIRE( PumpUntil( { source, stray }, source, kUserMessage ).peer == source );

    const RakNetGUID used = ConnectThroughRoute( peers, source, endpoint );
    CHECK( used == router->GetMyGUID() );
    CheckReaches( peers, source, endpoint );

    // The stray never answers this query, so the source's Update drops it once the ping
    // timeout passes and routes through the router.
    CHECK( Route( peers, sourcePlugin, source, secondEndpoint ) == router->GetMyGUID() );
    CheckReaches( peers, source, secondEndpoint );

    router->DetachPlugin( &routerPlugin );
    source->DetachPlugin( &sourcePlugin );
    endpoint->DetachPlugin( &endpointPlugin );
    secondEndpoint->DetachPlugin( &secondEndpointPlugin );
}

// The source's Router2 has no request outstanding, so it drops the router's answer, but the
// endpoint hears of the punch from its Designated router.
TEST_CASE( "Router2 punches again over a live forwarded connection without cutting it", "[router2][network]" )
{
    Router2 routerPlugin, sourcePlugin, endpointPlugin;
    routerPlugin.SetMaximumForwardingRequests( 4 );

    PeerScope scope;
    RakPeerInterface* router = scope.Server( kRouterPort, 4 );
    RakPeerInterface* source = scope.Server( kSourcePort, 4 );
    RakPeerInterface* endpoint = scope.Server( kEndpointPort, 4 );
    router->AttachPlugin( &routerPlugin );
    source->AttachPlugin( &sourcePlugin );
    endpoint->AttachPlugin( &endpointPlugin );
    const std::vector<RakPeerInterface*> peers{ router, source, endpoint };

    Connect( peers, source, kRouterPort );
    Connect( peers, endpoint, kRouterPort );
    endpointPlugin.AddIntermediary( SystemAddress( "127.0.0.1", kRouterPort ) );
    REQUIRE( Route( peers, sourcePlugin, source, endpoint, true ) == router->GetMyGUID() );
    const SystemAddress sourceSeesEndpointAt = source->GetSystemAddressFromGuid( endpoint->GetMyGUID() );

    RequestForwarding( source, router, endpoint );
    REQUIRE( PumpUntil( peers, endpoint, ID_ROUTER_2_REROUTED ).peer == endpoint );

    CheckReaches( peers, source, endpoint );
    CheckReaches( peers, endpoint, source );
    CHECK( source->GetSystemAddressFromGuid( endpoint->GetMyGUID() ) == sourceSeesEndpointAt );

    router->DetachPlugin( &routerPlugin );
    source->DetachPlugin( &sourcePlugin );
    endpoint->DetachPlugin( &endpointPlugin );
}

// The re-punch reused the forwarded pair, so giving up on it leaves the pair to carry the
// connection.
TEST_CASE( "Router2 keeps a live forwarded connection when a punch over it times out", "[router2][network]" )
{
    // For the router's next Update to see its punch timed out.
    constexpr TimeMS kTimeoutMarginMs = 500;

    Router2 routerPlugin, sourcePlugin, endpointPlugin;
    routerPlugin.SetMaximumForwardingRequests( 4 );

    PeerScope scope;
    RakPeerInterface* router = scope.Server( kRouterPort, 4 );
    RakPeerInterface* source = scope.Server( kSourcePort, 4 );
    RakPeerInterface* endpoint = scope.Server( kEndpointPort, 4 );
    router->AttachPlugin( &routerPlugin );
    source->AttachPlugin( &sourcePlugin );
    endpoint->AttachPlugin( &endpointPlugin );
    const std::vector<RakPeerInterface*> peers{ router, source, endpoint };

    Connect( peers, source, kRouterPort );
    Connect( peers, endpoint, kRouterPort );
    REQUIRE( Route( peers, sourcePlugin, source, endpoint ) == router->GetMyGUID() );

    // Never pumping the endpoint makes the punch time out. Its Peer keeps the connection up
    // meanwhile, since that needs no Receive. The source's Router2 consumes the router's
    // refusal, so this waits out the router's timeout instead: 3 s plus eight of the larger
    // of its pings to the two ends.
    const int largerPing = (std::max)( { router->GetAveragePing( source->GetMyGUID() ), router->GetAveragePing( endpoint->GetMyGUID() ), 0 } );
    RequestForwarding( source, router, endpoint );
    PumpUntil( { router, source }, []( const Received& ) { return false; }, 3000 + 8 * (TimeMS)largerPing + kTimeoutMarginMs );

    CheckReaches( peers, source, endpoint );
    CheckReaches( peers, endpoint, source );

    router->DetachPlugin( &routerPlugin );
    source->DetachPlugin( &sourcePlugin );
    endpoint->DetachPlugin( &endpointPlugin );
}

// Only the source's own request is outstanding, so the source would drop a second success
// unseen. A second punch would also reach the endpoint, as a second ID_ROUTER_2_REROUTED.
TEST_CASE( "Router2 answers a request repeated during its punch once", "[router2][network]" )
{
    // Long enough for the request to reach the router, well short of the punch timeout.
    constexpr TimeMS kPunchStallMs = 1000;
    // Long enough for a second success or refusal sent with the first to arrive.
    constexpr TimeMS kSettleMs = 500;

    Router2 routerPlugin, sourcePlugin, endpointPlugin;
    routerPlugin.SetMaximumForwardingRequests( 4 );

    PeerScope scope;
    RakPeerInterface* router = scope.Server( kRouterPort, 4 );
    RakPeerInterface* source = scope.Server( kSourcePort, 4 );
    RakPeerInterface* endpoint = scope.Server( kEndpointPort, 4 );
    router->AttachPlugin( &routerPlugin );
    source->AttachPlugin( &sourcePlugin );
    endpoint->AttachPlugin( &endpointPlugin );
    const std::vector<RakPeerInterface*> peers{ router, source, endpoint };

    Connect( peers, source, kRouterPort );
    Connect( peers, endpoint, kRouterPort );
    endpointPlugin.AddIntermediary( SystemAddress( "127.0.0.1", kRouterPort ) );

    int established = 0;
    int noRoute = 0;
    int rerouted = 0;
    const auto count = [&]( const Received& each ) {
        if( each.peer == source && each.id == ID_ROUTER_2_FORWARDING_ESTABLISHED )
            established++;
        if( IsNoRoute( each, source, endpoint ) )
            noRoute++;
        if( each.peer == endpoint && each.id == ID_ROUTER_2_REROUTED )
            rerouted++;
        return false;
    };

    // Not pumping the endpoint holds the punch back while the repeat arrives.
    sourcePlugin.EstablishRouting( endpoint->GetMyGUID() );
    PumpUntil( { router, source }, count, kPunchStallMs );
    RequestForwarding( source, router, endpoint );
    PumpUntil( { router, source }, count, kPunchStallMs );
    REQUIRE( established == 0 );
    REQUIRE( noRoute == 0 );

    PumpUntil( peers, [&]( const Received& each ) {
        count( each );
        return established > 0 && rerouted > 0;
    } );
    PumpUntil( peers, count, kSettleMs );
    CHECK( established == 1 );
    CHECK( noRoute == 0 );
    CHECK( rerouted == 1 );

    router->DetachPlugin( &routerPlugin );
    source->DetachPlugin( &sourcePlugin );
    endpoint->DetachPlugin( &endpointPlugin );
}
