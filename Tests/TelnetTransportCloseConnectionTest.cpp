#include "InspectableTelnetTransport.h"
#include "LoopbackTCP.h"
#include "RakNetTypes.h"
#include "TCPInterface.h"
#include "WinsockScope.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>

/*
Pins that a connection the user closes frees its TelnetClient, and frees it once.

ConsoleServer's quit command has the remote close its own connection through
TelnetTransport::CloseConnection. TCPInterface::CloseConnection queues no lost event, and the
update thread reports loss only for an active entry, so no lost event ever came for such a
connection. Its TelnetClient kept an openConnections of 1 until Stop, and a remote could
grow remoteClients without bound by connecting from fresh ports and typing quit.

CloseConnection cannot just count the connection closed, because the update thread may have
detected the loss and queued its lost event already. Both would then count the one
connection, and after a same-address reconnect the reconnect's entry would be freed under
it. So TCPInterface::CloseConnection returns whether it was the one to close the connection;
the update thread's check, push and release happen under the entry's isActiveMutex, so
exactly one of the two reports it.

The clients here are raw sockets. The second case reconnects from the port it used before, so
it binds a fixed local port and closes abortively - SO_LINGER with a zero timeout sends a
RST - which leaves no TIME_WAIT behind.
*/

using namespace LoopbackTCP;
using namespace RakNet;

namespace {

// TCP, and distinct from every other test's ports; CreateListenSocket does not set
// SO_REUSEADDR before it binds, so no port is shared between cases.
constexpr unsigned short kUserCloseListenPort = 31037;
constexpr unsigned short kUserCloseClientPort = 31038;
constexpr unsigned short kLostQueuedListenPort = 31039;
constexpr unsigned short kLostQueuedClientPort = 31040;
constexpr unsigned short kReturnValueListenPort = 31041;
constexpr unsigned short kReturnValueClientPortA = 31042;
constexpr unsigned short kReturnValueClientPortB = 31043;

// How long a case watches for an event that must not come. The update thread polls every
// 30 ms, so this spans several of its passes.
constexpr TimeMS kQuietPeriod = 300;

// True if no lost event is drained within kQuietPeriod.
bool NoLostEventFollows( InspectableTelnetTransport& telnet )
{
    return !WaitFor( [&] { return telnet.HasLostConnection() != UNASSIGNED_SYSTEM_ADDRESS; }, kQuietPeriod );
}

} // namespace

TEST_CASE( "TelnetTransport frees a client whose connection the user closes", "[telnettransport][network]" )
{
    WinsockScope winsock;

    InspectableTelnetTransport telnet;
    REQUIRE( telnet.Start( kUserCloseListenPort, true ) );

    SystemAddress address = UNASSIGNED_SYSTEM_ADDRESS;
    Client client( kUserCloseListenPort, kUserCloseClientPort );
    REQUIRE( WaitFor( [&] { return ( address = telnet.HasNewIncomingConnection() ) != UNASSIGNED_SYSTEM_ADDRESS; } ) );
    REQUIRE( telnet.ClientCount() == 1 );

    // What ConsoleServer does on quit.
    telnet.CloseConnection( address );

    // Unfixed, the entry stayed until Stop.
    CHECK( telnet.ClientCount() == 0 );

    // No lost event comes for a connection the user closed, so nothing is counted twice.
    CHECK( NoLostEventFollows( telnet ) );
    CHECK( telnet.ClientCount() == 0 );

    client.Abort();
    telnet.Stop();
}

TEST_CASE( "TelnetTransport frees a client once when its lost event is queued before the user closes it", "[telnettransport][network]" )
{
    WinsockScope winsock;

    InspectableTelnetTransport telnet;
    REQUIRE( telnet.Start( kLostQueuedListenPort, true ) );

    SystemAddress address = UNASSIGNED_SYSTEM_ADDRESS;
    Client first( kLostQueuedListenPort, kLostQueuedClientPort );
    REQUIRE( WaitFor( [&] { return ( address = telnet.HasNewIncomingConnection() ) != UNASSIGNED_SYSTEM_ADDRESS; } ) );
    REQUIRE( telnet.ClientCount() == 1 );

    // The update thread detects the loss and queues its lost event; it is not drained yet.
    first.Abort();
    REQUIRE( WaitFor( [&] { return telnet.ConnectionCount() == 0; } ) );

    // The connection is already gone, so this close does not count it.
    telnet.CloseConnection( address );
    CHECK( telnet.ClientCount() == 1 );

    // Its lost event does, once.
    REQUIRE( WaitFor( [&] { return telnet.HasLostConnection() == address; } ) );
    CHECK( telnet.ClientCount() == 0 );

    // A reconnect from the same address keeps its entry: nothing counted the first
    // connection twice and left the reconnect's count one short.
    Client second( kLostQueuedListenPort, kLostQueuedClientPort );
    REQUIRE( WaitFor( [&] { return telnet.HasNewIncomingConnection() == address; } ) );
    CHECK( telnet.ClientCount() == 1 );

    const char line[] = "hello\n";
    second.SendAll( line, strlen( line ) );
    Packet* received = 0;
    CHECK( WaitFor( [&] { return ( received = telnet.Receive() ) != 0; } ) );
    if( received )
    {
        CHECK( strcmp( (const char*)received->data, "hello" ) == 0 );
        telnet.DeallocatePacket( received );
    }

    second.Abort();
    CHECK( WaitFor( [&] { return telnet.HasLostConnection() == address; } ) );
    CHECK( telnet.ClientCount() == 0 );

    telnet.Stop();
}

TEST_CASE( "TCPInterface::CloseConnection reports whether it closed the connection", "[tcpinterface][network]" )
{
    WinsockScope winsock;

    TCPInterface tcp;
    REQUIRE( tcp.Start( kReturnValueListenPort, 4 ) );

    SystemAddress open = UNASSIGNED_SYSTEM_ADDRESS;
    Client openClient( kReturnValueListenPort, kReturnValueClientPortA );
    REQUIRE( WaitFor( [&] { return ( open = tcp.HasNewIncomingConnection() ) != UNASSIGNED_SYSTEM_ADDRESS; } ) );

    SystemAddress lost = UNASSIGNED_SYSTEM_ADDRESS;
    Client lostClient( kReturnValueListenPort, kReturnValueClientPortB );
    REQUIRE( WaitFor( [&] { return ( lost = tcp.HasNewIncomingConnection() ) != UNASSIGNED_SYSTEM_ADDRESS; } ) );

    lostClient.Abort();
    REQUIRE( WaitFor( [&] { return tcp.GetConnectionCount() == 1; } ) );

    // Detected lost by the update thread, whether or not its event was drained.
    CHECK_FALSE( tcp.CloseConnection( lost ) );

    SystemAddress unknown;
    REQUIRE( unknown.FromStringExplicitPort( "127.0.0.1", 1 ) );
    CHECK_FALSE( tcp.CloseConnection( unknown ) );
    CHECK_FALSE( tcp.CloseConnection( UNASSIGNED_SYSTEM_ADDRESS ) );

    CHECK( tcp.CloseConnection( open ) );
    CHECK( tcp.GetConnectionCount() == 0 );

    // Already closed by the call above.
    CHECK_FALSE( tcp.CloseConnection( open ) );

    openClient.Abort();
    tcp.Stop();
}
