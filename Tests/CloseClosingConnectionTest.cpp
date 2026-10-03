#include "ConnectionWaits.h"
#include "GetTime.h"
#include "PeerScope.h"
#include "RakNetStringMakers.h"
#include "RakNetTypes.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

/*
Pins that asking to close a closing connection record changes nothing (CONTEXT.md).

B receives A's disconnection notification and owes A an ack for it. The view can be one
update cycle old (ADR-0007), so B's application can still ask to close the connection at that
moment. That close leaves the one already running alone: B sends its ack and A's close ends at
once, not at A's timeout.

RakPeerInterface functions explicitly tested:

    CloseConnection
*/

using namespace RakNet;

namespace {

// Ports no other test uses.
constexpr unsigned short kPortA = 32500;
constexpr unsigned short kPortB = 32501;

// Hang guard for the connect and for B to start closing, not a settle time.
constexpr TimeMS kWaitBudgetMs = 10000;

// Many times the 10 ms an ack is held, and a few round trips on loopback.
constexpr TimeMS kQuietMs = 200;

// Far inside the 10 s Release and 30 s Debug timeout a close ends at when its notification
// is never acked, and far outside a close on loopback, which takes tens of milliseconds.
constexpr TimeMS kCloseBudgetMs = 1000;

} // namespace

TEST_CASE( "Closing a connection the System is already closing still acks the System's notification", "[network]" )
{
    PeerScope peers;
    RakPeerInterface* a = peers.Server( kPortA );
    RakPeerInterface* b = peers.Server( kPortB );
    const SystemAddress addressA( "127.0.0.1", kPortA );
    const SystemAddress addressB( "127.0.0.1", kPortB );

    REQUIRE( a->Connect( "127.0.0.1", kPortB, 0, 0 ) == CONNECTION_ATTEMPT_STARTED );
    TimeMS deadline = GetTimeMS() + kWaitBudgetMs;
    while( ( a->GetConnectionState( addressB ) != IS_CONNECTED || b->GetConnectionState( addressA ) != IS_CONNECTED ) &&
           ConnectionWaits::Expired( deadline ) == false )
        std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
    REQUIRE( a->GetConnectionState( addressB ) == IS_CONNECTED );
    REQUIRE( b->GetConnectionState( addressA ) == IS_CONNECTED );
    // Lets the traffic that follows a connect, its pings and their acks, die down. An ack B
    // still holds for any of it would go out with the one for A's notification, early.
    std::this_thread::sleep_for( std::chrono::milliseconds( kQuietMs ) );
    ConnectionWaits::Drain( a );
    ConnectionWaits::Drain( b );

    a->CloseConnection( addressB, true );

    // No sleep: B's close has to land while B still holds the ack back.
    deadline = GetTimeMS() + kWaitBudgetMs;
    while( b->GetConnectionState( addressA ) != IS_DISCONNECTING && ConnectionWaits::Expired( deadline ) == false )
    {
    }
    REQUIRE( b->GetConnectionState( addressA ) == IS_DISCONNECTING );
    // IMMEDIATE_PRIORITY wakes B's network thread now. Left to its own pace, B's next
    // update cycle comes as the ack's hold runs out and sends the ack before the close.
    b->CloseConnection( addressA, true, 0, IMMEDIATE_PRIORITY );

    const TimeMS closeStarted = GetTimeMS();
    deadline = closeStarted + kCloseBudgetMs;
    while( a->GetConnectionState( addressB ) != IS_NOT_CONNECTED && ConnectionWaits::Expired( deadline ) == false )
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );

    INFO( "A's state toward B after " << ( GetTimeMS() - closeStarted ) << " ms" );
    CHECK( a->GetConnectionState( addressB ) == IS_NOT_CONNECTED );
}
