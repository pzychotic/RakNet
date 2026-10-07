#include "Plugins/Router2.h"

#include "BitStream.h"
#include "ConnectionWaits.h"
#include "GetTime.h"
#include "MarkerInjection.h"
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

Each claim is checked through MarkerInjection.
ChangeSystemAddress only queues a command for the update thread, so every check on an address
first waits until a command queued behind it shows in the getters.
*/

using namespace RakNet;
using MarkerInjection::Contains;

namespace {

constexpr unsigned short kServerPort = 30000;
constexpr unsigned short kIntermediaryPort = 30001;
constexpr unsigned short kEndpointPort = 30002;

// Hang guard for a queued command and for a disconnection notification. On loopback each
// shows a few update cycles after it is queued or sent, tens of milliseconds.
constexpr TimeMS kStepBudgetMs = 5000;

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

// Returns once peer's getters show every command queued before the call. Buffered commands
// are applied in the order they were queued, so once a timeout change queued after them shows
// on peer's connection to other, they have all been applied.
bool WaitForQueuedCommands( RakPeerInterface* peer, RakNetGUID other )
{
    const SystemAddress otherAddress = peer->GetSystemAddressFromGuid( other );
    // UNASSIGNED_SYSTEM_ADDRESS would change the Peer-wide default, which shows at once.
    if( otherAddress == UNASSIGNED_SYSTEM_ADDRESS )
        return false;
    const TimeMS timeout = peer->GetTimeoutTime( otherAddress ) ^ 1;
    peer->SetTimeoutTime( timeout, otherAddress );

    const TimeMS deadline = GetTimeMS() + kStepBudgetMs;
    while( peer->GetTimeoutTime( otherAddress ) != timeout )
    {
        if( ConnectionWaits::Expired( deadline ) )
            return false;
        std::this_thread::sleep_for( std::chrono::milliseconds( ConnectionWaits::kPollInterval ) );
    }
    return true;
}

// Injects messageId, endpointGuid, port from sender into target, and returns what target's
// Receive handed out before the marker. Returns once every address change the Message
// queued has been applied.
std::vector<MessageID> Claim( RakPeerInterface* sender, RakPeerInterface* target, MessageID messageId, RakNetGUID endpointGuid, unsigned short port )
{
    BitStream claim;
    claim.Write( messageId );
    claim.Write( endpointGuid );
    claim.Write( port );
    std::vector<MessageID> received = MarkerInjection::Inject( sender, target, claim );
    REQUIRE( WaitForQueuedCommands( target, sender->GetMyGUID() ) );
    return received;
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

    ConnectionWaits::ConnectAndWait( intermediary, server );
    const SystemAddress intermediaryAddress = server->GetSystemAddressFromGuid( intermediary->GetMyGUID() );

    SECTION( "An undesignated System cannot move a connection or add an entry" )
    {
        ConnectionWaits::ConnectAndWait( endpoint, server );
        const SystemAddress endpointAddress = server->GetSystemAddressFromGuid( endpoint->GetMyGUID() );

        const std::vector<MessageID> live = Claim( intermediary, server, ID_ROUTER_2_REROUTED, endpoint->GetMyGUID(), 25000 );
        CHECK( !Contains( live, ID_ROUTER_2_REROUTED ) );
        CHECK( server->GetSystemAddressFromGuid( endpoint->GetMyGUID() ) == endpointAddress );

        const std::vector<MessageID> fresh = Claim( intermediary, server, ID_ROUTER_2_REROUTED, kUnconnectedGuid, 25001 );
        CHECK( !Contains( fresh, ID_ROUTER_2_REROUTED ) );
        CHECK( router.ForwardedCount() == 0 );
    }

    SECTION( "A Designated intermediary cannot move a direct connection" )
    {
        router.AddIntermediary( intermediaryAddress );
        ConnectionWaits::ConnectAndWait( endpoint, server );
        const SystemAddress endpointAddress = server->GetSystemAddressFromGuid( endpoint->GetMyGUID() );

        const std::vector<MessageID> received = Claim( intermediary, server, ID_ROUTER_2_REROUTED, endpoint->GetMyGUID(), 25000 );
        CHECK( !Contains( received, ID_ROUTER_2_REROUTED ) );
        CHECK( server->GetSystemAddressFromGuid( endpoint->GetMyGUID() ) == endpointAddress );
        CHECK( router.ForwardedCount() == 0 );
    }

    SECTION( "A Designated intermediary records a new forwarded connection" )
    {
        router.AddIntermediary( intermediaryAddress );

        const std::vector<MessageID> received = Claim( intermediary, server, ID_ROUTER_2_REROUTED, kUnconnectedGuid, 25001 );
        CHECK( Contains( received, ID_ROUTER_2_REROUTED ) );
        CHECK( router.ForwardedCount() == 1 );
    }

    SECTION( "A Designated intermediary moves a live connection that is already forwarded" )
    {
        router.AddIntermediary( intermediaryAddress );

        // Announced before it connects, at the address it will connect from: that is what a
        // connection forwarded by this intermediary looks like from here.
        Claim( intermediary, server, ID_ROUTER_2_REROUTED, endpoint->GetMyGUID(), kEndpointPort );
        REQUIRE( router.ForwardedCount() == 1 );

        ConnectionWaits::ConnectAndWait( endpoint, server );
        REQUIRE( server->GetSystemAddressFromGuid( endpoint->GetMyGUID() ) == Loopback( kEndpointPort ) );

        const std::vector<MessageID> rerouted = Claim( intermediary, server, ID_ROUTER_2_REROUTED, endpoint->GetMyGUID(), 25002 );
        CHECK( Contains( rerouted, ID_ROUTER_2_REROUTED ) );
        CHECK( server->GetSystemAddressFromGuid( endpoint->GetMyGUID() ) == Loopback( 25002 ) );
        CHECK( router.ForwardedCount() == 1 );
    }

    SECTION( "A Designated intermediary's announcement that arrives after its connection still counts" )
    {
        router.AddIntermediary( intermediaryAddress );
        ConnectionWaits::ConnectAndWait( endpoint, server );

        // Already at the announced address, so nothing moves, but the entry is recorded.
        const std::vector<MessageID> announced = Claim( intermediary, server, ID_ROUTER_2_REROUTED, endpoint->GetMyGUID(), kEndpointPort );
        CHECK( Contains( announced, ID_ROUTER_2_REROUTED ) );
        CHECK( server->GetSystemAddressFromGuid( endpoint->GetMyGUID() ) == Loopback( kEndpointPort ) );
        CHECK( router.ForwardedCount() == 1 );

        const std::vector<MessageID> rerouted = Claim( intermediary, server, ID_ROUTER_2_REROUTED, endpoint->GetMyGUID(), 25002 );
        CHECK( Contains( rerouted, ID_ROUTER_2_REROUTED ) );
        CHECK( server->GetSystemAddressFromGuid( endpoint->GetMyGUID() ) == Loopback( 25002 ) );
    }

    SECTION( "A Designated intermediary cannot move a connection whose entry is stale" )
    {
        router.AddIntermediary( intermediaryAddress );

        // Announced at a port the endpoint then does not connect from.
        Claim( intermediary, server, ID_ROUTER_2_REROUTED, endpoint->GetMyGUID(), kEndpointPort + 1 );
        REQUIRE( router.ForwardedCount() == 1 );

        ConnectionWaits::ConnectAndWait( endpoint, server );
        const SystemAddress endpointAddress = server->GetSystemAddressFromGuid( endpoint->GetMyGUID() );

        const std::vector<MessageID> rerouted = Claim( intermediary, server, ID_ROUTER_2_REROUTED, endpoint->GetMyGUID(), 25002 );
        CHECK( !Contains( rerouted, ID_ROUTER_2_REROUTED ) );
        CHECK( server->GetSystemAddressFromGuid( endpoint->GetMyGUID() ) == endpointAddress );
    }

    SECTION( "RemoveIntermediary withdraws the designation" )
    {
        router.AddIntermediary( intermediaryAddress );
        router.RemoveIntermediary( intermediaryAddress );

        const std::vector<MessageID> received = Claim( intermediary, server, ID_ROUTER_2_REROUTED, kUnconnectedGuid, 25001 );
        CHECK( !Contains( received, ID_ROUTER_2_REROUTED ) );
        CHECK( router.ForwardedCount() == 0 );
    }

    SECTION( "A designation lapses when its connection closes" )
    {
        router.AddIntermediary( intermediaryAddress );

        Claim( intermediary, server, ID_ROUTER_2_REROUTED, kUnconnectedGuid, 25001 );
        REQUIRE( router.ForwardedCount() == 1 );

        intermediary->CloseConnection( Loopback( kServerPort ), true );
        ConnectionWaits::WaitForDisconnect( intermediary, Loopback( kServerPort ) );
        REQUIRE( ConnectionWaits::WaitForMessage( server, ID_DISCONNECTION_NOTIFICATION, kStepBudgetMs ) );

        // The closed connection's entry for an endpoint that never connected goes with it.
        CHECK( router.ForwardedCount() == 0 );

        // The same address again, but not the same designation.
        ConnectionWaits::ConnectAndWait( intermediary, server );
        REQUIRE( server->GetSystemAddressFromGuid( intermediary->GetMyGUID() ) == intermediaryAddress );

        const std::vector<MessageID> received = Claim( intermediary, server, ID_ROUTER_2_REROUTED, RakNetGUID( 1002 ), 25001 );
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

    ConnectionWaits::ConnectAndWait( intermediary, server );
    router.AddIntermediary( server->GetSystemAddressFromGuid( intermediary->GetMyGUID() ) );
    router.SetMaxPendingForwardsPerIntermediary( 2 );
    CHECK( router.GetMaxPendingForwardsPerIntermediary() == 2 );

    for( uint64_t g = 1; g <= 3; g++ )
    {
        const std::vector<MessageID> received = Claim( intermediary, server, ID_ROUTER_2_REROUTED, RakNetGUID( 1000 + g ), 25000 );
        CHECK( Contains( received, ID_ROUTER_2_REROUTED ) == ( g <= 2 ) );
    }
    CHECK( router.ForwardedCount() == 2 );
    CHECK( router.GetPendingForwardsRefused() == 1 );

    // A repeat announcement of an entry it already holds is not a new one.
    const std::vector<MessageID> repeated = Claim( intermediary, server, ID_ROUTER_2_REROUTED, RakNetGUID( 1001 ), 25005 );
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

    ConnectionWaits::ConnectAndWait( asked, server );
    ConnectionWaits::ConnectAndWait( other, server );

    SECTION( "for a new forwarded connection" )
    {
        router.AddRequest( kUnconnectedGuid, asked->GetMyGUID() );

        const std::vector<MessageID> forged = Claim( other, server, ID_ROUTER_2_FORWARDING_ESTABLISHED, kUnconnectedGuid, 25000 );
        CHECK( !Contains( forged, ID_ROUTER_2_FORWARDING_ESTABLISHED ) );
        CHECK( router.HasRequest( kUnconnectedGuid ) );
        CHECK( router.ForwardedCount() == 0 );

        const std::vector<MessageID> genuine = Claim( asked, server, ID_ROUTER_2_FORWARDING_ESTABLISHED, kUnconnectedGuid, 25000 );
        CHECK( Contains( genuine, ID_ROUTER_2_FORWARDING_ESTABLISHED ) );
        CHECK( !router.HasRequest( kUnconnectedGuid ) );
        CHECK( router.ForwardedCount() == 1 );
    }

    SECTION( "for a live forwarded connection" )
    {
        ConnectionWaits::ConnectAndWait( endpoint, server );
        const SystemAddress endpointAddress = server->GetSystemAddressFromGuid( endpoint->GetMyGUID() );
        router.AddInitiatedForwarding( endpoint->GetMyGUID(), asked->GetMyGUID(), endpointAddress );

        // Nothing asked at all.
        const std::vector<MessageID> unasked = Claim( other, server, ID_ROUTER_2_FORWARDING_ESTABLISHED, endpoint->GetMyGUID(), 25000 );
        CHECK( !Contains( unasked, ID_ROUTER_2_FORWARDING_ESTABLISHED ) );
        CHECK( !Contains( unasked, ID_ROUTER_2_REROUTED ) );
        CHECK( server->GetSystemAddressFromGuid( endpoint->GetMyGUID() ) == endpointAddress );

        // A re-route in progress, answered by a System that was not asked.
        router.AddRequest( endpoint->GetMyGUID(), asked->GetMyGUID() );
        const std::vector<MessageID> forged = Claim( other, server, ID_ROUTER_2_FORWARDING_ESTABLISHED, endpoint->GetMyGUID(), 25000 );
        CHECK( !Contains( forged, ID_ROUTER_2_REROUTED ) );
        CHECK( server->GetSystemAddressFromGuid( endpoint->GetMyGUID() ) == endpointAddress );
        CHECK( router.HasRequest( endpoint->GetMyGUID() ) );

        const std::vector<MessageID> genuine = Claim( asked, server, ID_ROUTER_2_FORWARDING_ESTABLISHED, endpoint->GetMyGUID(), 25003 );
        CHECK( Contains( genuine, ID_ROUTER_2_REROUTED ) );
        CHECK( server->GetSystemAddressFromGuid( endpoint->GetMyGUID() ) == Loopback( 25003 ) );
        CHECK( !router.HasRequest( endpoint->GetMyGUID() ) );
        CHECK( router.ForwardedCount() == 1 );
    }

    server->DetachPlugin( &router );
}
