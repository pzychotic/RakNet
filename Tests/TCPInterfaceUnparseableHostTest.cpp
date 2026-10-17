#include "ConnectionWaits.h"
#include "RakNetTypes.h"
#include "TCPInterface.h"

#include <catch2/catch_test_macros.hpp>

/*
TCPInterface::Connect rejects a host it can't parse before it starts an attempt: it returns
UNASSIGNED_SYSTEM_ADDRESS, reserves no slot and queues no failed attempt, in both branches.
A bad host string is a caller error, not a failed connect.

Both branches used to go ahead with whatever address the failed parse left. The blocking one
connected to 0.0.0.0, which on Linux reaches the local machine, so the listener here took the
connection; on Windows the connect fails and the attempt was queued as failed. The
non-blocking one started a connect thread on UNASSIGNED_SYSTEM_ADDRESS that holds its slot
until the connect gives up, tens of seconds later.

The client has one slot, so a valid connect straight after the rejected one succeeds only if
the rejected one reserved nothing. Stop clears the queued attempts, so they are read before
it.

The host is numeric with a stray character after it, so the parse fails without a DNS lookup.
*/

using namespace RakNet;

namespace {

// Ports no other test uses.
constexpr unsigned short kListenPort = 31071;

const char* const kUnparseableHost = "127.0.0.1!";

// Long enough for the listener's update thread to have accepted a loopback connection.
constexpr TimeMS kAcceptSettleMs = 500;

bool NewIncomingConnection( TCPInterface& server, TimeMS budget )
{
    return ConnectionWaits::WaitUntil( [&] { return server.HasNewIncomingConnection() != UNASSIGNED_SYSTEM_ADDRESS; }, budget );
}

void CheckRejectedWithoutAttempt( bool block )
{
    TCPInterface server;
    REQUIRE( server.Start( kListenPort, 4 ) );

    TCPInterface client;
    REQUIRE( client.Start( 0, 0, 1 ) );

    CHECK( client.Connect( kUnparseableHost, kListenPort, block ) == UNASSIGNED_SYSTEM_ADDRESS );
    CHECK( client.HasFailedConnectionAttempt() == UNASSIGNED_SYSTEM_ADDRESS );

    CHECK( client.Connect( "127.0.0.1", kListenPort, true ) != UNASSIGNED_SYSTEM_ADDRESS );

    // The valid connect's, and no other.
    CHECK( NewIncomingConnection( server, kAcceptSettleMs ) );
    CHECK_FALSE( NewIncomingConnection( server, kAcceptSettleMs ) );

    client.Stop();
    server.Stop();
}

} // namespace

TEST_CASE( "Connect rejects an unparseable host without an attempt, blocking", "[tcpinterface][network]" )
{
    CheckRejectedWithoutAttempt( true );
}

TEST_CASE( "Connect rejects an unparseable host without an attempt, non-blocking", "[tcpinterface][network]" )
{
    CheckRejectedWithoutAttempt( false );
}
