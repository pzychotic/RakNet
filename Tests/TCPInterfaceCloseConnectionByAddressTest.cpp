#include "TCPInterface.h"
#include "RakNetTypes.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

/*
Pins which remoteClients entry TCPInterface::CloseConnection closes when the address it is
given does not carry a systemIndex that names the connection.

CloseConnection has a fast path - systemIndex is in range and the entry there matches the
address, so release that entry - and a fallback search for everything else. The defect was in
the fallback: it locked and tested remoteClients[i], and then cleared
remoteClients[systemAddress.systemIndex], a different entry by construction, since the fast
path had already handled the case where those two agree. The entry the caller named stayed
open, an unrelated one was closed in its place, and the write happened without that entry's
own isActiveMutex held. systemIndex is unvalidated on this path as well - the fast path
bounds-checks it and the fallback is reached because that check failed, which includes
failing on the bound - so an unassigned systemIndex of 65535 indexed far past the array.

SystemAddress::operator== ignores systemIndex, so an address is a fine argument to
CloseConnection without a meaningful one; that is what the fallback search exists for. The
two cases below are the two ways a caller ordinarily holds such an address.

The first is deterministic and needs no allocator help: with two live connections it asserts
which one survives rather than how many do, so the unfixed code fails it by closing the wrong
one, in range and without crashing. The second is the out-of-bounds half - an unassigned
systemIndex - where the unfixed code calls a member function on an object that was never
constructed, so it fails by whatever means the allocator or the operating system chooses
before it can reach the assertion.
*/

using namespace RakNet;

namespace {

// TCP, so these share no space with the UDP ports the rest of the suite hardcodes, and
// distinct from the other TCPInterface tests' ports so a stray listener is never ambiguous
// about which test left it. Two listeners for the first case, because a connection is only
// distinguishable from another by the address it was made to; one for the second. CreateListenSocket
// does not set SO_REUSEADDR before it binds, so no port is shared between cases: a TIME_WAIT
// left by one could fail the next one's Start for no reason of its own.
constexpr unsigned short kStaleIndexPortA = 61021;
constexpr unsigned short kStaleIndexPortB = 61022;
constexpr unsigned short kUnsetIndexPort = 61023;

// Loopback, so every wait here is over as soon as the two threads have been scheduled once.
// Generous so a loaded machine cannot turn a pass into a failure.
constexpr std::chrono::milliseconds kDeadline( 5000 );

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

// The addresses of every connection the interface still holds open.
std::vector<SystemAddress> ConnectionList( const TCPInterface& tcpInterface )
{
    std::vector<SystemAddress> addresses( 8 );
    unsigned short count = (unsigned short)addresses.size();
    tcpInterface.GetConnectionList( addresses.data(), &count );
    addresses.resize( count );
    return addresses;
}

bool Holds( const std::vector<SystemAddress>& addresses, const SystemAddress& address )
{
    return std::find( addresses.begin(), addresses.end(), address ) != addresses.end();
}

} // namespace

TEST_CASE( "CloseConnection by an address whose systemIndex names another connection", "[tcpinterface][network]" )
{
    // Two listeners, so the client's two connections differ by the port they were made to
    // and each is nameable on its own.
    TCPInterface serverA;
    REQUIRE( serverA.Start( kStaleIndexPortA, 4 ) );
    TCPInterface serverB;
    REQUIRE( serverB.Start( kStaleIndexPortB, 4 ) );

    TCPInterface client;
    REQUIRE( client.Start( 0, 0, 2 ) );

    // Slot 0 and slot 1, in that order: the claim takes the first free entry.
    const SystemAddress addressA = client.Connect( "127.0.0.1", kStaleIndexPortA, true, AF_INET );
    REQUIRE( addressA != UNASSIGNED_SYSTEM_ADDRESS );
    const SystemAddress addressB = client.Connect( "127.0.0.1", kStaleIndexPortB, true, AF_INET );
    REQUIRE( addressB != UNASSIGNED_SYSTEM_ADDRESS );
    REQUIRE( addressA.systemIndex != addressB.systemIndex );
    REQUIRE( client.GetConnectionCount() == 2 );

    // The address of the second connection, carrying the first one's slot number. This is
    // what a caller holds after a peer disconnects and reconnects into a different slot:
    // the address still compares equal - operator== ignores systemIndex - but the index it
    // carries now names somebody else's live connection.
    SystemAddress staleIndexed = addressB;
    staleIndexed.systemIndex = addressA.systemIndex;

    client.CloseConnection( staleIndexed );

    // The fast path is not taken: remoteClients[addressA.systemIndex] holds addressA, which
    // does not compare equal to addressB. So the fallback search runs, matches the entry
    // holding addressB, and - unfixed - clears the entry named by the stale index instead,
    // closing connection A and leaving connection B open. Exactly backwards.
    const std::vector<SystemAddress> remaining = ConnectionList( client );
    CHECK( remaining.size() == 1 );
    CHECK( Holds( remaining, addressA ) );
    CHECK_FALSE( Holds( remaining, addressB ) );

    client.Stop();
    serverB.Stop();
    serverA.Stop();
}

TEST_CASE( "CloseConnection by an address with no systemIndex", "[tcpinterface][network]" )
{
    TCPInterface server;
    REQUIRE( server.Start( kUnsetIndexPort, 4 ) );

    TCPInterface client;
    REQUIRE( client.Start( 0, 0, 1 ) );

    REQUIRE( client.Connect( "127.0.0.1", kUnsetIndexPort, true, AF_INET ) != UNASSIGNED_SYSTEM_ADDRESS );
    REQUIRE( client.GetConnectionCount() == 1 );
    REQUIRE( WaitFor( [&server] { return server.HasNewIncomingConnection() != UNASSIGNED_SYSTEM_ADDRESS; } ) );

    // Built from a string rather than kept from Connect, which is the other ordinary way to
    // hold an address for a connection you have. systemIndex is whatever SystemAddress's
    // constructor left, i.e. unassigned.
    SystemAddress unindexed;
    REQUIRE( unindexed.FromStringExplicitPort( "127.0.0.1", kUnsetIndexPort ) );
    REQUIRE( unindexed.systemIndex == UNASSIGNED_SYSTEM_ADDRESS.systemIndex );

    // The load-bearing call. 65535 fails the fast path's bounds check, so the fallback runs
    // and - unfixed - reaches remoteClients[65535], calling SetActive on an object past the
    // end of an array of length 1. Under a debug allocator or ASan it fails here rather
    // than returning.
    client.CloseConnection( unindexed );

    CHECK( client.GetConnectionCount() == 0 );
    CHECK( ConnectionList( client ).empty() );

    // And the connection really is gone rather than merely unlisted: the far end sees the
    // socket close.
    CHECK( WaitFor( [&server] { return server.HasLostConnection() != UNASSIGNED_SYSTEM_ADDRESS; } ) );

    client.Stop();
    server.Stop();
}
