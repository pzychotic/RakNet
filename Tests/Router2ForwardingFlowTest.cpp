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

constexpr unsigned short kRouterPort = 31000;
constexpr unsigned short kSecondRouterPort = 31001;
constexpr unsigned short kSourcePort = 31002;
constexpr unsigned short kEndpointPort = 31003;

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

void Connect( const std::vector<RakPeerInterface*>& peers, RakPeerInterface* client, unsigned short port )
{
    REQUIRE( client->Connect( "127.0.0.1", port, nullptr, 0 ) == CONNECTION_ATTEMPT_STARTED );
    REQUIRE( PumpUntil( peers, client, ID_CONNECTION_REQUEST_ACCEPTED ).peer == client );
}

// Sends a user Message from one end to the other by RakNetGUID, and checks it arrives.
void CheckReaches( const std::vector<RakPeerInterface*>& peers, RakPeerInterface* from, RakPeerInterface* to )
{
    BitStream bs;
    bs.Write( kUserMessage );
    from->Send( &bs, HIGH_PRIORITY, RELIABLE_ORDERED, 0, to->GetMyGUID(), false );
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
// Returns the router's RakNetGUID.
RakNetGUID ConnectThroughRoute( const std::vector<RakPeerInterface*>& peers, RakPeerInterface* source, RakPeerInterface* endpoint )
{
    const Received established = PumpUntil( peers, [source, endpoint]( const Received& each ) {
        return ( each.peer == source && each.id == ID_ROUTER_2_FORWARDING_ESTABLISHED ) || IsNoRoute( each, source, endpoint );
    } );
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
RakNetGUID Route( const std::vector<RakPeerInterface*>& peers, Router2& sourceRouter, RakPeerInterface* source, RakPeerInterface* endpoint )
{
    sourceRouter.EstablishRouting( endpoint->GetMyGUID() );
    return ConnectThroughRoute( peers, source, endpoint );
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
