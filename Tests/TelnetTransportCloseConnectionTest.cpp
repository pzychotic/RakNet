#include "Plugins/TelnetTransport.h"
#include "RakNetTypes.h"
#include "SocketDefines.h"
#include "SocketIncludes.h"
#include "TCPInterface.h"
#include "WSAStartupSingleton.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstring>
#include <thread>

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

// Loopback, so every wait here is over as soon as the threads have been scheduled once.
// Generous so a loaded machine cannot turn a pass into a failure.
constexpr std::chrono::milliseconds kDeadline( 5000 );

// How long a case watches for an event that must not come. The update thread polls every
// 30 ms, so this spans several of its passes.
constexpr std::chrono::milliseconds kQuietPeriod( 300 );

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

// Nothing here goes through RakPeer::Startup, so the raw sockets take the same Winsock
// refcount RakNet itself does. A no-op off Windows.
struct WinsockScope
{
    WinsockScope() { WSAStartupSingleton::AddRef(); }
    ~WinsockScope() { WSAStartupSingleton::Deref(); }
};

// remoteClients and tcpInterface are protected; the test needs a count from each.
class InspectableTelnetTransport : public TelnetTransport
{
public:
    size_t ClientCount() const { return remoteClients.size(); }

    // Connections TCPInterface holds active, whether or not their events were drained.
    unsigned short ActiveCount() const { return tcpInterface->GetConnectionCount(); }
};

sockaddr_in LoopbackAddress( unsigned short port )
{
    sockaddr_in address;
    memset( &address, 0, sizeof( address ) );
    address.sin_family = AF_INET;
    address.sin_port = htons( port );
    address.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
    return address;
}

// A blocking TCP connection to the listener, made from a fixed local port.
__TCPSOCKET__ ConnectFromFixedPort( unsigned short listenPort, unsigned short clientPort )
{
    const __TCPSOCKET__ s = socket__( AF_INET, SOCK_STREAM, IPPROTO_TCP );
    REQUIRE( s != (__TCPSOCKET__)-1 );

    const int reuse = 1;
    REQUIRE( setsockopt__( s, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof( reuse ) ) == 0 );

    const sockaddr_in local = LoopbackAddress( clientPort );
    REQUIRE( bind__( s, (const sockaddr*)&local, sizeof( local ) ) == 0 );

    const sockaddr_in remote = LoopbackAddress( listenPort );
    REQUIRE( connect__( s, (const sockaddr*)&remote, sizeof( remote ) ) == 0 );
    return s;
}

// Closes with a RST rather than a FIN, so the local port is free to bind again at once.
void Abort( __TCPSOCKET__ s )
{
    linger abortive;
    abortive.l_onoff = 1;
    abortive.l_linger = 0;
    setsockopt__( s, SOL_SOCKET, SO_LINGER, (const char*)&abortive, sizeof( abortive ) );
    closesocket__( s );
}

// True if no lost event is drained within kQuietPeriod.
bool NoLostEventFollows( InspectableTelnetTransport& telnet )
{
    const auto deadline = std::chrono::steady_clock::now() + kQuietPeriod;
    while( std::chrono::steady_clock::now() < deadline )
    {
        if( telnet.HasLostConnection() != UNASSIGNED_SYSTEM_ADDRESS )
            return false;
        std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
    }
    return true;
}

} // namespace

TEST_CASE( "TelnetTransport frees a client whose connection the user closes", "[telnettransport][network]" )
{
    WinsockScope winsock;

    InspectableTelnetTransport telnet;
    REQUIRE( telnet.Start( kUserCloseListenPort, true ) );

    SystemAddress address = UNASSIGNED_SYSTEM_ADDRESS;
    const __TCPSOCKET__ client = ConnectFromFixedPort( kUserCloseListenPort, kUserCloseClientPort );
    REQUIRE( WaitFor( [&] { return ( address = telnet.HasNewIncomingConnection() ) != UNASSIGNED_SYSTEM_ADDRESS; } ) );
    REQUIRE( telnet.ClientCount() == 1 );

    // What ConsoleServer does on quit.
    telnet.CloseConnection( address );

    // Unfixed, the entry stayed until Stop.
    CHECK( telnet.ClientCount() == 0 );

    // No lost event comes for a connection the user closed, so nothing is counted twice.
    CHECK( NoLostEventFollows( telnet ) );
    CHECK( telnet.ClientCount() == 0 );

    Abort( client );
    telnet.Stop();
}

TEST_CASE( "TelnetTransport frees a client once when its lost event is queued before the user closes it", "[telnettransport][network]" )
{
    WinsockScope winsock;

    InspectableTelnetTransport telnet;
    REQUIRE( telnet.Start( kLostQueuedListenPort, true ) );

    SystemAddress address = UNASSIGNED_SYSTEM_ADDRESS;
    const __TCPSOCKET__ first = ConnectFromFixedPort( kLostQueuedListenPort, kLostQueuedClientPort );
    REQUIRE( WaitFor( [&] { return ( address = telnet.HasNewIncomingConnection() ) != UNASSIGNED_SYSTEM_ADDRESS; } ) );
    REQUIRE( telnet.ClientCount() == 1 );

    // The update thread detects the loss and queues its lost event; it is not drained yet.
    Abort( first );
    REQUIRE( WaitFor( [&] { return telnet.ActiveCount() == 0; } ) );

    // The connection is already gone, so this close does not count it.
    telnet.CloseConnection( address );
    CHECK( telnet.ClientCount() == 1 );

    // Its lost event does, once.
    REQUIRE( WaitFor( [&] { return telnet.HasLostConnection() == address; } ) );
    CHECK( telnet.ClientCount() == 0 );

    // A reconnect from the same address keeps its entry: nothing counted the first
    // connection twice and left the reconnect's count one short.
    const __TCPSOCKET__ second = ConnectFromFixedPort( kLostQueuedListenPort, kLostQueuedClientPort );
    REQUIRE( WaitFor( [&] { return telnet.HasNewIncomingConnection() == address; } ) );
    CHECK( telnet.ClientCount() == 1 );

    const char line[] = "hello\n";
    REQUIRE( send__( second, line, (int)strlen( line ), 0 ) == (int)strlen( line ) );
    Packet* received = 0;
    CHECK( WaitFor( [&] { return ( received = telnet.Receive() ) != 0; } ) );
    if( received )
    {
        CHECK( strcmp( (const char*)received->data, "hello" ) == 0 );
        telnet.DeallocatePacket( received );
    }

    Abort( second );
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
    const __TCPSOCKET__ openClient = ConnectFromFixedPort( kReturnValueListenPort, kReturnValueClientPortA );
    REQUIRE( WaitFor( [&] { return ( open = tcp.HasNewIncomingConnection() ) != UNASSIGNED_SYSTEM_ADDRESS; } ) );

    SystemAddress lost = UNASSIGNED_SYSTEM_ADDRESS;
    const __TCPSOCKET__ lostClient = ConnectFromFixedPort( kReturnValueListenPort, kReturnValueClientPortB );
    REQUIRE( WaitFor( [&] { return ( lost = tcp.HasNewIncomingConnection() ) != UNASSIGNED_SYSTEM_ADDRESS; } ) );

    Abort( lostClient );
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

    Abort( openClient );
    tcp.Stop();
}
