#include "TCPInterface.h"
#include "RakNetTypes.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <thread>

/*
Pins the length contract of TCPInterface::Connect's bindAddress parameter.

The defect: the non-blocking arm copied the caller's bindAddress into
ThisPtrPlusSysAddr::bindAddress - a char[64] that is not the last member of the struct -
with a plain strcpy and no bound. A 64-character-or-longer argument, which nothing in the
signature or the header comment ruled out, overran the array on the heap. The first thing
past it is socketFamily, read back on the connect thread and handed to SocketConnect, so
even a one-byte overrun changes the address family of the connect before it reaches the
heap block's own metadata.

The fix rejects an over-long argument at the top of Connect rather than truncating it:
truncation would bind to an address the caller did not ask for, and Connect already has a
failure return, so per ADR-0002 that is the channel. The check sits ahead of the
remoteClients slot loop, so a rejected call does not strand a slot.

Only the first case can catch the original overflow. The other two are the boundary either
side of it: 63 characters plus NUL is the longest argument that still fits, and it has to
keep working through both arms - the non-blocking one because that is where the two copies
live, the blocking one because it never copied and so must be unaffected either way.

The 64-character rejection needs no peer: it happens before any network call. The two
acceptance cases connect to a loopback listener, because acceptance is not otherwise
observable - the non-blocking arm returns UNASSIGNED_SYSTEM_ADDRESS whether it accepted the
argument or not, so a completed connection attempt is the only proof it got that far.
*/

using namespace RakNet;

namespace {

// TCP, so it shares no space with the UDP ports the rest of the suite hardcodes; distinct
// from them regardless, so a stray listener is never ambiguous about which test left it.
constexpr unsigned short kListenPort = 61010;

// A connect to a listening socket on loopback is immediate; this only has to be longer than
// a scheduler hiccup, and is generous so a loaded machine cannot turn a pass into a failure.
constexpr std::chrono::milliseconds kConnectDeadline( 5000 );

// Both lengths are derived from the bound rather than written out, so a change to the array
// moves the boundary this file tests with it.
const std::string kLongestAccepted( TCPInterface::MAXIMUM_BIND_ADDRESS_LENGTH, '1' );

// One over: the string that overran the array before the fix.
const std::string kFirstRejected( TCPInterface::MAXIMUM_BIND_ADDRESS_LENGTH + 1, '1' );

// Runs the interface's update thread until the connection attempt completes or the deadline
// passes. Returns the completed address, or UNASSIGNED_SYSTEM_ADDRESS on the deadline.
SystemAddress WaitForCompletedConnectionAttempt( TCPInterface& tcpInterface )
{
    const auto deadline = std::chrono::steady_clock::now() + kConnectDeadline;

    while( std::chrono::steady_clock::now() < deadline )
    {
        const SystemAddress completed = tcpInterface.HasCompletedConnectionAttempt();
        if( completed != UNASSIGNED_SYSTEM_ADDRESS )
            return completed;

        if( tcpInterface.HasFailedConnectionAttempt() != UNASSIGNED_SYSTEM_ADDRESS )
            return UNASSIGNED_SYSTEM_ADDRESS;

        std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
    }

    return UNASSIGNED_SYSTEM_ADDRESS;
}

} // namespace

TEST_CASE( "Connect rejects a bindAddress that does not fit and claims no slot", "[tcpinterface]" )
{
    TCPInterface client;

    // No incoming connections, so no listen socket and no port of its own: this case never
    // reaches a network call.
    REQUIRE( client.Start( 0, 0, 4 ) );

    REQUIRE( client.GetConnectionCount() == 0 );

    // Against the unfixed code this is the heap overflow, on the way in, before the argument
    // is ever looked at.
    CHECK( client.Connect( "127.0.0.1", kListenPort, false, AF_INET, kFirstRejected.c_str() ) == UNASSIGNED_SYSTEM_ADDRESS );

    // The load-bearing assertion. The non-blocking arm returns UNASSIGNED_SYSTEM_ADDRESS on
    // success too, so the return value above cannot tell the fix from the defect; the slot
    // the unfixed code claims - before spawning the thread that would have used it - can.
    CHECK( client.GetConnectionCount() == 0 );

    // The blocking arm shares the check, and there the return value is the whole signal.
    CHECK( client.Connect( "127.0.0.1", kListenPort, true, AF_INET, kFirstRejected.c_str() ) == UNASSIGNED_SYSTEM_ADDRESS );
    CHECK( client.GetConnectionCount() == 0 );

    client.Stop();
}

// [network] as well, per the suite's rule: this one binds a listening socket and completes a
// real loopback connection. The rejection case above deliberately does not carry the tag.
TEST_CASE( "Connect accepts the longest bindAddress that fits, blocking", "[tcpinterface][network]" )
{
    TCPInterface server;
    REQUIRE( server.Start( kListenPort, 4 ) );

    TCPInterface client;
    REQUIRE( client.Start( 0, 0, 4 ) );

    // The blocking arm never copied bindAddress, so it was never the overflow; this says the
    // check added ahead of both arms did not cost it an argument it took before.
    const SystemAddress connected = client.Connect( "127.0.0.1", kListenPort, true, AF_INET, kLongestAccepted.c_str() );
    CHECK( connected != UNASSIGNED_SYSTEM_ADDRESS );

    client.Stop();
    server.Stop();
}

TEST_CASE( "Connect accepts the longest bindAddress that fits, non-blocking", "[tcpinterface][network]" )
{
    TCPInterface server;
    REQUIRE( server.Start( kListenPort, 4 ) );

    TCPInterface client;
    REQUIRE( client.Start( 0, 0, 4 ) );

    // This is the arm with the two copies in it - into the struct here, and out of it again
    // on the connect thread. A bound written one byte short would truncate this address, and
    // a bound written one byte long would still overrun; reaching a completed attempt says
    // the string survived both copies at the largest size they have to carry.
    CHECK( client.Connect( "127.0.0.1", kListenPort, false, AF_INET, kLongestAccepted.c_str() ) == UNASSIGNED_SYSTEM_ADDRESS );

    CHECK( WaitForCompletedConnectionAttempt( client ) != UNASSIGNED_SYSTEM_ADDRESS );

    client.Stop();
    server.Stop();
}
