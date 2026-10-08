#include "Plugins/Router2.h"

#include "PeerScope.h"
#include "RakNetStringMakers.h"
#include "RakNetTypes.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

/*
Router2 asks the router with the lowest ping to the endpoint, weighted by how many forwarding
entries it already uses, to forward. Of routers that score the same, it asks the one listed
first.

The ordering function put a router that scored higher than another before it, so which
router was asked depended on the order the pings came back in.
*/

using namespace RakNet;

namespace {

// Reaches RequestForwarding, so a request can be handed routers with known pings. The peer
// is started and connected to nothing, so the request it sends goes nowhere.
class Router2Probe : public Router2
{
public:
    RakNetGUID ChosenRouter( const std::vector<ConnectionRequestSystem>& routers )
    {
        ConnnectRequest request;
        request.requestState = R2RS_REQUEST_STATE_QUERY_FORWARDING;
        request.endpointGuid = RakNetGUID( 100 );
        request.lastRequestedForwardingSystem = UNASSIGNED_RAKNET_GUID;
        request.returnConnectionLostOnFailure = false;
        request.connectionRequestSystems = routers;
        RequestForwarding( &request );
        return request.lastRequestedForwardingSystem;
    }
};

Router2::ConnectionRequestSystem Router( uint64_t guid, int pingToEndpoint, unsigned short usedForwardingEntries )
{
    Router2::ConnectionRequestSystem router;
    router.guid = RakNetGUID( guid );
    router.pingToEndpoint = pingToEndpoint;
    router.usedForwardingEntries = usedForwardingEntries;
    return router;
}

} // namespace

TEST_CASE( "Router2 asks the router with the lowest weighted ping to forward", "[router2]" )
{
    Router2Probe router2;

    PeerScope peers;
    RakPeerInterface* peer = peers.Client();
    peer->AttachPlugin( &router2 );

    CHECK( router2.ChosenRouter( { Router( 1, 5, 0 ), Router( 2, 10, 0 ) } ) == RakNetGUID( 1 ) );
    CHECK( router2.ChosenRouter( { Router( 2, 10, 0 ), Router( 1, 5, 0 ) } ) == RakNetGUID( 1 ) );
    // 6 * 2 against 10 * 1
    CHECK( router2.ChosenRouter( { Router( 1, 6, 1 ), Router( 2, 10, 0 ) } ) == RakNetGUID( 2 ) );
    CHECK( router2.ChosenRouter( { Router( 3, 9, 0 ), Router( 1, 6, 1 ), Router( 2, 10, 0 ) } ) == RakNetGUID( 3 ) );

    peer->DetachPlugin( &router2 );
}

TEST_CASE( "Router2 asks the router listed first of those with the same weighted ping", "[router2]" )
{
    Router2Probe router2;

    PeerScope peers;
    RakPeerInterface* peer = peers.Client();
    peer->AttachPlugin( &router2 );

    // 10 * 1 against 5 * 2
    CHECK( router2.ChosenRouter( { Router( 2, 10, 0 ), Router( 1, 5, 1 ) } ) == RakNetGUID( 2 ) );
    CHECK( router2.ChosenRouter( { Router( 1, 5, 1 ), Router( 2, 10, 0 ) } ) == RakNetGUID( 1 ) );

    peer->DetachPlugin( &router2 );
}
