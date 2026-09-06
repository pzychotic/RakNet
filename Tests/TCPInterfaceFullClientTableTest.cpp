#include "TCPInterface.h"
#include "RakNetTypes.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

/*
Pins what TCPInterface does when every remoteClients slot is already taken.

The defect: both slot searches - Connect's, and the accept path's in
UpdateTCPInterfaceLoop - declared their index as -1 and then immediately reused that same
variable as the loop counter, so the -1 was overwritten before it was ever compared. The
loop has two exits, break with a valid index and fall-through with the index equal to
remoteClientsLength, and neither is -1; the guard that tested for -1 could not fire.

In Connect that meant a full table fell through to remoteClients[remoteClientsLength] - one
element past the array - and locked a std::mutex and wrote members of an object that was
never constructed. In the accept path the writes all sit inside the isActive == false
branch, so a full table wrote nothing out of bounds but also never stored and never closed
the socket it had just accepted, leaking a descriptor per connection for as long as the
table stayed full. Making that guard live is only half the fix: the branch behind it closed
sts->listenSocket, the server's own listener, where it meant to close the socket it could
not house.

The three cases below are the three consequences. The first two are Connect's arms; both
need a real listener, because a slot is only durably occupied by a connection that actually
completes. The third is the accept path, and it observes the close from the far side: a
client whose connection the server accepted and then closed sees the connection lost, where
the leaking code leaves it open indefinitely. Its final connect is what says the listener
itself survived - the half fix, making the guard live without correcting the argument to
closesocket__, takes the listener down on the first connection past capacity and that last
connect never completes.
*/

using namespace RakNet;

namespace {

// TCP, so these share no space with the UDP ports the rest of the suite hardcodes; distinct
// from the other TCPInterface test's port so a stray listener is never ambiguous about which
// test left it. One per case rather than one for the file: every case here completes real
// connections, and CreateListenSocket does not set SO_REUSEADDR before it binds, so a
// TIME_WAIT left by the previous case could fail the next one's Start for no reason of its
// own.
constexpr unsigned short kBlockingListenPort = 61011;
constexpr unsigned short kNonBlockingListenPort = 61012;
constexpr unsigned short kAcceptListenPort = 61013;

// Loopback, so every wait here is over as soon as the two threads have been scheduled once.
// Generous so a loaded machine cannot turn a pass into a failure.
constexpr std::chrono::milliseconds kDeadline( 5000 );

// Long enough that the connect thread the unfixed code spawns would have finished and
// reported; short enough to pay once. Only used where the expected answer is "nothing
// happens", so it is a lower bound on patience rather than a timeout.
constexpr std::chrono::milliseconds kQuietPeriod( 500 );

// Runs until the predicate holds or the deadline passes; returns whether it held.
template <typename Predicate>
bool WaitFor( Predicate predicate )
{
    const auto deadline = std::chrono::steady_clock::now() + kDeadline;

    while( std::chrono::steady_clock::now() < deadline )
    {
        if( predicate() )
            return true;

        std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
    }

    return predicate();
}

} // namespace

TEST_CASE( "Connect refuses a full client table, blocking", "[tcpinterface][network]" )
{
    TCPInterface server;
    REQUIRE( server.Start( kBlockingListenPort, 4 ) );

    // One outgoing slot and no listener: the table is full after a single connection.
    TCPInterface client;
    REQUIRE( client.Start( 0, 0, 1 ) );

    REQUIRE( client.Connect( "127.0.0.1", kBlockingListenPort, true, AF_INET ) != UNASSIGNED_SYSTEM_ADDRESS );
    REQUIRE( client.GetConnectionCount() == 1 );

    // The load-bearing call. Against the unfixed code this walks off the end of the array:
    // it locks remoteClients[1].isActiveMutex - a mutex on an object that does not exist -
    // and writes .socket and .systemAddress through it. Under a debug allocator or ASan it
    // fails here rather than returning.
    CHECK( client.Connect( "127.0.0.1", kBlockingListenPort, true, AF_INET ) == UNASSIGNED_SYSTEM_ADDRESS );

    CHECK( client.GetConnectionCount() == 1 );

    client.Stop();
    server.Stop();
}

TEST_CASE( "Connect refuses a full client table, non-blocking", "[tcpinterface][network]" )
{
    TCPInterface server;
    REQUIRE( server.Start( kNonBlockingListenPort, 4 ) );

    TCPInterface client;
    REQUIRE( client.Start( 0, 0, 1 ) );

    REQUIRE( client.Connect( "127.0.0.1", kNonBlockingListenPort, true, AF_INET ) != UNASSIGNED_SYSTEM_ADDRESS );
    REQUIRE( client.GetConnectionCount() == 1 );

    // The blocking arm queues its success as a completed attempt as well as returning it.
    // Take that one off the queue here, so the queue being empty below says something about
    // the refused call rather than about this one.
    REQUIRE( client.HasCompletedConnectionAttempt() != UNASSIGNED_SYSTEM_ADDRESS );

    // The non-blocking arm returns UNASSIGNED_SYSTEM_ADDRESS whether it took the call or
    // refused it, so this line cannot tell the fix from the defect on its own.
    CHECK( client.Connect( "127.0.0.1", kNonBlockingListenPort, false, AF_INET ) == UNASSIGNED_SYSTEM_ADDRESS );

    // This can. The unfixed code stashes remoteClientsLength in the address's systemIndex
    // and hands it to a connect thread, which does the same out-of-bounds write off the
    // update thread's timeline and then reports the attempt one way or the other. A refusal
    // spawns no thread, so nothing is ever reported.
    std::this_thread::sleep_for( kQuietPeriod );
    CHECK( client.HasCompletedConnectionAttempt() == UNASSIGNED_SYSTEM_ADDRESS );
    CHECK( client.HasFailedConnectionAttempt() == UNASSIGNED_SYSTEM_ADDRESS );

    CHECK( client.GetConnectionCount() == 1 );

    client.Stop();
    server.Stop();
}

TEST_CASE( "A connection arriving at a full table is closed and the listener survives", "[tcpinterface][network]" )
{
    // Two incoming connections allowed at the listen socket, but only one slot to house
    // them, so the second one accepted is the one the accept path has nowhere to put.
    TCPInterface server;
    REQUIRE( server.Start( kAcceptListenPort, 2, 1 ) );

    TCPInterface occupant;
    REQUIRE( occupant.Start( 0, 0, 1 ) );
    REQUIRE( occupant.Connect( "127.0.0.1", kAcceptListenPort, true, AF_INET ) != UNASSIGNED_SYSTEM_ADDRESS );
    REQUIRE( WaitFor( [&server] { return server.HasNewIncomingConnection() != UNASSIGNED_SYSTEM_ADDRESS; } ) );

    {
        // The server's table is full. accept__ still succeeds - the listen backlog is not
        // the client table - so this connect completes at the TCP level either way; what
        // differs is what the server does with the socket afterwards.
        TCPInterface refused;
        REQUIRE( refused.Start( 0, 0, 1 ) );
        REQUIRE( refused.Connect( "127.0.0.1", kAcceptListenPort, true, AF_INET ) != UNASSIGNED_SYSTEM_ADDRESS );

        // The fix closes that socket, which the far end sees as the connection ending. The
        // unfixed code holds it open with no owner and no way to ever close it.
        CHECK( WaitFor( [&refused] { return refused.HasLostConnection() != UNASSIGNED_SYSTEM_ADDRESS; } ) );

        refused.Stop();
    }

    // Free the slot, and the interface has to accept again.
    occupant.Stop();
    REQUIRE( WaitFor( [&server] { return server.HasLostConnection() != UNASSIGNED_SYSTEM_ADDRESS; } ) );

    // The assertion the half fix fails: closing listenSocket instead of the accepted socket
    // ends the server's ability to accept anything the moment one connection arrives past
    // capacity, so this last connection is never seen.
    TCPInterface late;
    REQUIRE( late.Start( 0, 0, 1 ) );
    REQUIRE( late.Connect( "127.0.0.1", kAcceptListenPort, true, AF_INET ) != UNASSIGNED_SYSTEM_ADDRESS );
    CHECK( WaitFor( [&server] { return server.HasNewIncomingConnection() != UNASSIGNED_SYSTEM_ADDRESS; } ) );

    late.Stop();
    server.Stop();
}
