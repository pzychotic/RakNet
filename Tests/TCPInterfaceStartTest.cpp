#include "ConnectionWaits.h"
#include "LoopbackTCP.h"
#include "TCPInterface.h"

#include <catch2/catch_test_macros.hpp>

/*
Pins that TCPInterface::Start listens on the port it was given, or says it could not.

Start ignored CreateListenSocket's result, so a listen bind that failed still returned
true, with no listen socket behind it. Every connection to the port was then refused, and
nothing said why.

The bind failed on Linux whenever an earlier connection on the port had not finished
closing, because the listen socket did not set SO_REUSEADDR. A server that closes its end
first holds the port in TIME_WAIT, and one whose last ACK is still owed holds it in
LAST_ACK. Either kept a restarted server from listening.
*/

using namespace RakNet;

namespace {

// TCP, so it shares no space with the UDP ports the rest of the suite hardcodes, and
// distinct from the other TCPInterface tests' ports.
constexpr unsigned short kHeldPort = 31017;
constexpr unsigned short kRestartPort = 31018;

} // namespace

TEST_CASE( "TCPInterface Start reports a port another listener holds", "[tcpinterface][network]" )
{
    TCPInterface holder;
    REQUIRE( holder.Start( kHeldPort, 4 ) );

    TCPInterface second;
    CHECK( second.Start( kHeldPort, 4 ) == false );

    // A failed Start leaves the interface stopped, so it starts once the port is free.
    second.Stop();
    holder.Stop();
    REQUIRE( second.Start( kHeldPort, 4 ) );
    LoopbackTCP::Client client( kHeldPort );
    second.Stop();
}

TEST_CASE( "TCPInterface Start listens again on a port whose last connection it closed first", "[tcpinterface][network]" )
{
    TCPInterface first;
    REQUIRE( first.Start( kRestartPort, 4 ) );
    // Outlives both servers: its abortive close would end the old connection's TIME_WAIT,
    // and the restart has to happen while it lasts.
    LoopbackTCP::Client oldClient( kRestartPort );
    REQUIRE( ConnectionWaits::WaitUntil( [&] { return first.GetConnectionCount() == 1; }, LoopbackTCP::kWaitBudget ) );

    // The server closes first, so its end of the connection is the one left in TIME_WAIT.
    first.Stop();

    TCPInterface restarted;
    REQUIRE( restarted.Start( kRestartPort, 4 ) );
    LoopbackTCP::Client newClient( kRestartPort );
    CHECK( ConnectionWaits::WaitUntil( [&] { return restarted.GetConnectionCount() == 1; }, LoopbackTCP::kWaitBudget ) );
    restarted.Stop();
}
