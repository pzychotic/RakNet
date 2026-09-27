#include "Plugins/Router2.h"

#include "BitStream.h"
#include "ConnectionWaits.h"
#include "GetTime.h"
#include "MessageIdentifiers.h"
#include "PeerScope.h"
#include "RakMemoryOverride.h"
#include "RakNetStringMakers.h"
#include "RakNetTypes.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>
#include <vector>

/*
Router2 acts on a "forwarding is set up" claim only if it is Solicited or comes from a
Designated System (ADR-0006).

The source side takes ID_ROUTER_2_FORWARDING_ESTABLISHED only from the router it asked, for
an endpoint it has a request outstanding for. The endpoint side takes ID_ROUTER_2_REROUTED
only from a System the application designated with AddIntermediary, and moves a live
connection only if that connection is already forwarded. Stock took both from any connected
System and moved any connection to <sender's IP>:<port of its choosing>.

Each injected Message is followed by a user Message on the same ordered channel, so once the
user Message comes out of Receive the injected one has been through the plugin.
ChangeSystemAddress only queues a command for the update thread, so every check on an address
first calls GetSockets, which queues a command behind it and blocks until it is answered.
*/

using namespace RakNet;

namespace {

constexpr unsigned short kServerPort = 60000;
constexpr unsigned short kIntermediaryPort = 60001;
constexpr unsigned short kEndpointPort = 60002;

// Hang guard for the marker Message. On loopback it arrives a few update cycles after the
// send, tens of milliseconds.
constexpr TimeMS kMarkerBudgetMs = 5000;

constexpr MessageID kMarker = ID_USER_PACKET_ENUM;

const RakNetGUID kUnconnectedGuid( 1001 );

// Exposes the plugin's tables, so a test can stand in for a request it would have made and
// count the entries a Message left behind.
class InspectableRouter2 : public Router2
{
public:
    // A request for endpointGuid that has asked router to forward, as RequestForwarding
    // leaves it.
    void AddRequest( RakNetGUID endpointGuid, RakNetGUID router )
    {
        ConnnectRequest* request = RakNet::OP_NEW<ConnnectRequest>( _FILE_AND_LINE_ );
        request->requestState = REQUEST_STATE_REQUEST_FORWARDING;
        request->pingTimeout = 0;
        request->endpointGuid = endpointGuid;
        request->lastRequestedForwardingSystem = router;
        request->returnConnectionLostOnFailure = false;
        std::lock_guard<std::mutex> guard( connectionRequestsMutex );
        connectionRequests.push_back( request );
    }

    bool HasRequest( RakNetGUID endpointGuid )
    {
        std::lock_guard<std::mutex> guard( connectionRequestsMutex );
        return GetConnectionRequestIndex( endpointGuid ) != ~0u;
    }

    // A connection this Peer routed to endpointGuid through intermediaryAddress.
    void AddInitiatedForwarding( RakNetGUID endpointGuid, RakNetGUID intermediaryGuid, const SystemAddress& intermediaryAddress )
    {
        ForwardedConnection fc;
        fc.endpointGuid = endpointGuid;
        fc.intermediaryGuid = intermediaryGuid;
        fc.intermediaryAddress = intermediaryAddress;
        fc.returnConnectionLostOnFailure = false;
        fc.weInitiatedForwarding = true;
        std::lock_guard<std::mutex> guard( forwardedConnectionListMutex );
        forwardedConnectionList.push_back( fc );
    }

    size_t ForwardedCount()
    {
        std::lock_guard<std::mutex> guard( forwardedConnectionListMutex );
        return forwardedConnectionList.size();
    }
};

// Returns the message ids the peer's Receive hands out up to and including the marker, or
// up to the deadline if the marker never comes.
std::vector<MessageID> ReceiveUntilMarker( RakPeerInterface* peer )
{
    std::vector<MessageID> received;
    const TimeMS deadline = GetTimeMS() + kMarkerBudgetMs;
    while( !ConnectionWaits::Expired( deadline ) )
    {
        for( Packet* packet = peer->Receive(); packet != nullptr; packet = peer->Receive() )
        {
            received.push_back( packet->data[0] );
            peer->DeallocatePacket( packet );
        }
        if( !received.empty() && received.back() == kMarker )
            break;
        std::this_thread::sleep_for( std::chrono::milliseconds( ConnectionWaits::kPollInterval ) );
    }
    return received;
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

bool Contains( const std::vector<MessageID>& ids, MessageID id )
{
    for( MessageID each : ids )
    {
        if( each == id )
            return true;
    }
    return false;
}

// Sends messageId, endpointGuid, port from sender to target, then the marker, and returns
// what target's Receive handed out. Returns once every address change the Message queued
// has been applied.
std::vector<MessageID> Inject( RakPeerInterface* sender, RakPeerInterface* target, MessageID messageId, RakNetGUID endpointGuid, unsigned short port )
{
    const SystemAddress targetAddress = sender->GetSystemAddressFromGuid( target->GetMyGUID() );

    BitStream forged;
    forged.Write( messageId );
    forged.Write( endpointGuid );
    forged.Write( port );
    sender->Send( &forged, HIGH_PRIORITY, RELIABLE_ORDERED, 0, targetAddress, false );

    BitStream marker;
    marker.Write( kMarker );
    sender->Send( &marker, HIGH_PRIORITY, RELIABLE_ORDERED, 0, targetAddress, false );

    std::vector<MessageID> received = ReceiveUntilMarker( target );

    std::vector<RakNetSocket2*> sockets;
    target->GetSockets( sockets );
    return received;
}

void Connect( RakPeerInterface* client, RakPeerInterface* server )
{
    REQUIRE( client->Connect( "127.0.0.1", kServerPort, nullptr, 0 ) == CONNECTION_ATTEMPT_STARTED );
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

TEST_CASE( "Router2 takes a reroute only from a Designated intermediary", "[router2][network]" )
{
    // Before the PeerScope, so it outlives the peers.
    InspectableRouter2 router;

    PeerScope peers;
    RakPeerInterface* server = peers.Server( kServerPort, 4 );
    RakPeerInterface* intermediary = peers.Client( kIntermediaryPort );
    RakPeerInterface* endpoint = peers.Client( kEndpointPort );
    server->AttachPlugin( &router );

    Connect( intermediary, server );
    const SystemAddress intermediaryAddress = server->GetSystemAddressFromGuid( intermediary->GetMyGUID() );

    SECTION( "An undesignated System cannot move a connection or add an entry" )
    {
        Connect( endpoint, server );
        const SystemAddress endpointAddress = server->GetSystemAddressFromGuid( endpoint->GetMyGUID() );

        const std::vector<MessageID> live = Inject( intermediary, server, ID_ROUTER_2_REROUTED, endpoint->GetMyGUID(), 40000 );
        REQUIRE( Contains( live, kMarker ) );
        CHECK( !Contains( live, ID_ROUTER_2_REROUTED ) );
        CHECK( server->GetSystemAddressFromGuid( endpoint->GetMyGUID() ) == endpointAddress );

        const std::vector<MessageID> fresh = Inject( intermediary, server, ID_ROUTER_2_REROUTED, kUnconnectedGuid, 40001 );
        REQUIRE( Contains( fresh, kMarker ) );
        CHECK( !Contains( fresh, ID_ROUTER_2_REROUTED ) );
        CHECK( router.ForwardedCount() == 0 );
    }

    SECTION( "A Designated intermediary cannot move a direct connection" )
    {
        router.AddIntermediary( intermediaryAddress );
        Connect( endpoint, server );
        const SystemAddress endpointAddress = server->GetSystemAddressFromGuid( endpoint->GetMyGUID() );

        const std::vector<MessageID> received = Inject( intermediary, server, ID_ROUTER_2_REROUTED, endpoint->GetMyGUID(), 40000 );
        REQUIRE( Contains( received, kMarker ) );
        CHECK( !Contains( received, ID_ROUTER_2_REROUTED ) );
        CHECK( server->GetSystemAddressFromGuid( endpoint->GetMyGUID() ) == endpointAddress );
        CHECK( router.ForwardedCount() == 0 );
    }

    SECTION( "A Designated intermediary records a new forwarded connection" )
    {
        router.AddIntermediary( intermediaryAddress );

        const std::vector<MessageID> received = Inject( intermediary, server, ID_ROUTER_2_REROUTED, kUnconnectedGuid, 40001 );
        REQUIRE( Contains( received, kMarker ) );
        CHECK( Contains( received, ID_ROUTER_2_REROUTED ) );
        CHECK( router.ForwardedCount() == 1 );
    }

    SECTION( "A Designated intermediary moves a live connection that is already forwarded" )
    {
        router.AddIntermediary( intermediaryAddress );

        // Announced before it connects, at the address it will connect from: that is what a
        // connection forwarded by this intermediary looks like from here.
        const std::vector<MessageID> announced = Inject( intermediary, server, ID_ROUTER_2_REROUTED, endpoint->GetMyGUID(), kEndpointPort );
        REQUIRE( Contains( announced, kMarker ) );
        REQUIRE( router.ForwardedCount() == 1 );

        Connect( endpoint, server );
        REQUIRE( server->GetSystemAddressFromGuid( endpoint->GetMyGUID() ) == Loopback( kEndpointPort ) );

        const std::vector<MessageID> rerouted = Inject( intermediary, server, ID_ROUTER_2_REROUTED, endpoint->GetMyGUID(), 40002 );
        REQUIRE( Contains( rerouted, kMarker ) );
        CHECK( Contains( rerouted, ID_ROUTER_2_REROUTED ) );
        CHECK( server->GetSystemAddressFromGuid( endpoint->GetMyGUID() ) == Loopback( 40002 ) );
        CHECK( router.ForwardedCount() == 1 );
    }

    SECTION( "A Designated intermediary's announcement that arrives after its connection still counts" )
    {
        router.AddIntermediary( intermediaryAddress );
        Connect( endpoint, server );

        // Already at the announced address, so nothing moves, but the entry is recorded.
        const std::vector<MessageID> announced = Inject( intermediary, server, ID_ROUTER_2_REROUTED, endpoint->GetMyGUID(), kEndpointPort );
        REQUIRE( Contains( announced, kMarker ) );
        CHECK( Contains( announced, ID_ROUTER_2_REROUTED ) );
        CHECK( server->GetSystemAddressFromGuid( endpoint->GetMyGUID() ) == Loopback( kEndpointPort ) );
        CHECK( router.ForwardedCount() == 1 );

        const std::vector<MessageID> rerouted = Inject( intermediary, server, ID_ROUTER_2_REROUTED, endpoint->GetMyGUID(), 40002 );
        REQUIRE( Contains( rerouted, kMarker ) );
        CHECK( Contains( rerouted, ID_ROUTER_2_REROUTED ) );
        CHECK( server->GetSystemAddressFromGuid( endpoint->GetMyGUID() ) == Loopback( 40002 ) );
    }

    SECTION( "A Designated intermediary cannot move a connection whose entry is stale" )
    {
        router.AddIntermediary( intermediaryAddress );

        // Announced at a port the endpoint then does not connect from.
        const std::vector<MessageID> announced = Inject( intermediary, server, ID_ROUTER_2_REROUTED, endpoint->GetMyGUID(), kEndpointPort + 1 );
        REQUIRE( Contains( announced, kMarker ) );
        REQUIRE( router.ForwardedCount() == 1 );

        Connect( endpoint, server );
        const SystemAddress endpointAddress = server->GetSystemAddressFromGuid( endpoint->GetMyGUID() );

        const std::vector<MessageID> rerouted = Inject( intermediary, server, ID_ROUTER_2_REROUTED, endpoint->GetMyGUID(), 40002 );
        REQUIRE( Contains( rerouted, kMarker ) );
        CHECK( !Contains( rerouted, ID_ROUTER_2_REROUTED ) );
        CHECK( server->GetSystemAddressFromGuid( endpoint->GetMyGUID() ) == endpointAddress );
    }

    SECTION( "RemoveIntermediary withdraws the designation" )
    {
        router.AddIntermediary( intermediaryAddress );
        router.RemoveIntermediary( intermediaryAddress );

        const std::vector<MessageID> received = Inject( intermediary, server, ID_ROUTER_2_REROUTED, kUnconnectedGuid, 40001 );
        REQUIRE( Contains( received, kMarker ) );
        CHECK( !Contains( received, ID_ROUTER_2_REROUTED ) );
        CHECK( router.ForwardedCount() == 0 );
    }

    SECTION( "A designation lapses when its connection closes" )
    {
        router.AddIntermediary( intermediaryAddress );

        const std::vector<MessageID> announced = Inject( intermediary, server, ID_ROUTER_2_REROUTED, kUnconnectedGuid, 40001 );
        REQUIRE( Contains( announced, kMarker ) );
        REQUIRE( router.ForwardedCount() == 1 );

        intermediary->CloseConnection( Loopback( kServerPort ), true );
        ConnectionWaits::WaitForDisconnect( intermediary, Loopback( kServerPort ) );
        REQUIRE( WaitForMessage( server, ID_DISCONNECTION_NOTIFICATION ) );

        // The closed connection's entry for an endpoint that never connected goes with it.
        CHECK( router.ForwardedCount() == 0 );

        // The same address again, but not the same designation.
        Connect( intermediary, server );
        REQUIRE( server->GetSystemAddressFromGuid( intermediary->GetMyGUID() ) == intermediaryAddress );

        const std::vector<MessageID> received = Inject( intermediary, server, ID_ROUTER_2_REROUTED, RakNetGUID( 1002 ), 40001 );
        REQUIRE( Contains( received, kMarker ) );
        CHECK( !Contains( received, ID_ROUTER_2_REROUTED ) );
        CHECK( router.ForwardedCount() == 0 );
    }

    server->DetachPlugin( &router );
}

TEST_CASE( "Router2 caps the connections a Designated intermediary announces ahead of their connection", "[router2][network]" )
{
    InspectableRouter2 router;

    PeerScope peers;
    RakPeerInterface* server = peers.Server( kServerPort, 4 );
    RakPeerInterface* intermediary = peers.Client( kIntermediaryPort );
    server->AttachPlugin( &router );

    Connect( intermediary, server );
    router.AddIntermediary( server->GetSystemAddressFromGuid( intermediary->GetMyGUID() ) );
    router.SetMaxPendingForwardsPerIntermediary( 2 );
    CHECK( router.GetMaxPendingForwardsPerIntermediary() == 2 );

    for( uint64_t g = 1; g <= 3; g++ )
    {
        const std::vector<MessageID> received = Inject( intermediary, server, ID_ROUTER_2_REROUTED, RakNetGUID( 1000 + g ), 40000 );
        REQUIRE( Contains( received, kMarker ) );
        CHECK( Contains( received, ID_ROUTER_2_REROUTED ) == ( g <= 2 ) );
    }
    CHECK( router.ForwardedCount() == 2 );
    CHECK( router.GetPendingForwardsRefused() == 1 );

    // A repeat announcement of an entry it already holds is not a new one.
    const std::vector<MessageID> repeated = Inject( intermediary, server, ID_ROUTER_2_REROUTED, RakNetGUID( 1001 ), 40005 );
    REQUIRE( Contains( repeated, kMarker ) );
    CHECK( Contains( repeated, ID_ROUTER_2_REROUTED ) );
    CHECK( router.ForwardedCount() == 2 );

    server->DetachPlugin( &router );
}

TEST_CASE( "Router2 takes a forwarding success only from the router it asked", "[router2][network]" )
{
    InspectableRouter2 router;

    PeerScope peers;
    RakPeerInterface* server = peers.Server( kServerPort, 4 );
    RakPeerInterface* asked = peers.Client( kIntermediaryPort );
    RakPeerInterface* other = peers.Client();
    RakPeerInterface* endpoint = peers.Client( kEndpointPort );
    server->AttachPlugin( &router );

    Connect( asked, server );
    Connect( other, server );

    SECTION( "for a new forwarded connection" )
    {
        router.AddRequest( kUnconnectedGuid, asked->GetMyGUID() );

        const std::vector<MessageID> forged = Inject( other, server, ID_ROUTER_2_FORWARDING_ESTABLISHED, kUnconnectedGuid, 40000 );
        REQUIRE( Contains( forged, kMarker ) );
        CHECK( !Contains( forged, ID_ROUTER_2_FORWARDING_ESTABLISHED ) );
        CHECK( router.HasRequest( kUnconnectedGuid ) );
        CHECK( router.ForwardedCount() == 0 );

        const std::vector<MessageID> genuine = Inject( asked, server, ID_ROUTER_2_FORWARDING_ESTABLISHED, kUnconnectedGuid, 40000 );
        REQUIRE( Contains( genuine, kMarker ) );
        CHECK( Contains( genuine, ID_ROUTER_2_FORWARDING_ESTABLISHED ) );
        CHECK( !router.HasRequest( kUnconnectedGuid ) );
        CHECK( router.ForwardedCount() == 1 );
    }

    SECTION( "for a live forwarded connection" )
    {
        Connect( endpoint, server );
        const SystemAddress endpointAddress = server->GetSystemAddressFromGuid( endpoint->GetMyGUID() );
        router.AddInitiatedForwarding( endpoint->GetMyGUID(), asked->GetMyGUID(), endpointAddress );

        // Nothing asked at all.
        const std::vector<MessageID> unasked = Inject( other, server, ID_ROUTER_2_FORWARDING_ESTABLISHED, endpoint->GetMyGUID(), 40000 );
        REQUIRE( Contains( unasked, kMarker ) );
        CHECK( !Contains( unasked, ID_ROUTER_2_FORWARDING_ESTABLISHED ) );
        CHECK( !Contains( unasked, ID_ROUTER_2_REROUTED ) );
        CHECK( server->GetSystemAddressFromGuid( endpoint->GetMyGUID() ) == endpointAddress );

        // A re-route in progress, answered by a System that was not asked.
        router.AddRequest( endpoint->GetMyGUID(), asked->GetMyGUID() );
        const std::vector<MessageID> forged = Inject( other, server, ID_ROUTER_2_FORWARDING_ESTABLISHED, endpoint->GetMyGUID(), 40000 );
        REQUIRE( Contains( forged, kMarker ) );
        CHECK( !Contains( forged, ID_ROUTER_2_REROUTED ) );
        CHECK( server->GetSystemAddressFromGuid( endpoint->GetMyGUID() ) == endpointAddress );
        CHECK( router.HasRequest( endpoint->GetMyGUID() ) );

        const std::vector<MessageID> genuine = Inject( asked, server, ID_ROUTER_2_FORWARDING_ESTABLISHED, endpoint->GetMyGUID(), 40003 );
        REQUIRE( Contains( genuine, kMarker ) );
        CHECK( Contains( genuine, ID_ROUTER_2_REROUTED ) );
        CHECK( server->GetSystemAddressFromGuid( endpoint->GetMyGUID() ) == Loopback( 40003 ) );
        CHECK( !router.HasRequest( endpoint->GetMyGUID() ) );
        CHECK( router.ForwardedCount() == 1 );
    }

    server->DetachPlugin( &router );
}
