#include "Plugins/UDPProxyCoordinator.h"

#include "GetTime.h"
#include "RakMemoryOverride.h"
#include "RakNetStringMakers.h"
#include "RakNetTypes.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

/*
UDPProxyCoordinator forgets only the forwarding requests that are done with, and keeps the
rest.

Update forgets a request once its success timeout has passed. OnClosedConnection forgets
every request the disconnected System made. In each case the removed request is the middle
one of three in sender address order, so a removal that shifted or skipped its neighbours in
a sorted list would lose the one after it.
*/

using namespace RakNet;

namespace {

using ForwardingRequest = UDPProxyCoordinator::ForwardingRequest;

const SystemAddress kFirstClient( "10.0.1.1", 2001 );
const SystemAddress kMiddleClient( "10.0.1.2", 2002 );
const SystemAddress kLastClient( "10.0.1.3", 2003 );
const SystemAddress kTargetClient( "10.0.2.1", 3001 );

// Reaches the forwarding request list, so requests can be set up in a given state without
// clients and logged-in proxy servers.
class CoordinatorProbe : public UDPProxyCoordinator
{
public:
    using UDPProxyCoordinator::CountRequestsFrom;

    // A request from requester to kTargetClient, sent to a server and not yet answered.
    ForwardingRequest* AddRequest( const SystemAddress& requester )
    {
        ForwardingRequest* fw = RakNet::OP_NEW<ForwardingRequest>( _FILE_AND_LINE_ );
        fw->timeoutOnNoDataMS = 0;
        fw->timeoutAfterSuccess = 0;
        fw->timeRequestedPings = 0;
        fw->sata.senderClientAddress = requester;
        fw->sata.targetClientAddress = kTargetClient;
        fw->requestingAddress = requester;
        forwardingRequestList.emplace( fw->sata, fw );
        return fw;
    }
};

} // namespace

TEST_CASE( "Update forgets a succeeded forwarding request and keeps the others", "[UDPProxyCoordinator]" )
{
    CoordinatorProbe coordinator;
    coordinator.AddRequest( kFirstClient );
    // Succeeded, and the wait for duplicates ends in a millisecond. GetTimeMS counts from
    // process start, and 0 means no success, so no fixed time is safely in the past.
    const TimeMS succeededUntil = GetTimeMS() + 1;
    coordinator.AddRequest( kMiddleClient )->timeoutAfterSuccess = succeededUntil;
    coordinator.AddRequest( kLastClient );
    while( GetTimeMS() <= succeededUntil )
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );

    coordinator.Update();

    CHECK( coordinator.CountRequestsFrom( kFirstClient ) == 1 );
    CHECK( coordinator.CountRequestsFrom( kMiddleClient ) == 0 );
    CHECK( coordinator.CountRequestsFrom( kLastClient ) == 1 );
}

TEST_CASE( "A requester disconnecting forgets its forwarding requests and keeps the others", "[UDPProxyCoordinator]" )
{
    CoordinatorProbe coordinator;
    coordinator.AddRequest( kFirstClient );
    coordinator.AddRequest( kMiddleClient );
    coordinator.AddRequest( kLastClient );

    coordinator.OnClosedConnection( kMiddleClient, UNASSIGNED_RAKNET_GUID, LCR_DISCONNECTION_NOTIFICATION );

    CHECK( coordinator.CountRequestsFrom( kFirstClient ) == 1 );
    CHECK( coordinator.CountRequestsFrom( kMiddleClient ) == 0 );
    CHECK( coordinator.CountRequestsFrom( kLastClient ) == 1 );
}
