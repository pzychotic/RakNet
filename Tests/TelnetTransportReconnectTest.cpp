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
Pins how TelnetTransport accounts for connections that share an address: a client
reconnecting from the address it last connected from, and a connection whose lost event is
drained before its new one.

TelnetTransport keeps a TelnetClient per remote address for line reassembly, and TCPInterface
gives it nothing better to key on. New and lost events come from two separate queues, so
their relative order is lost, and a reconnect accepted after the old connection's loss was
detected usually takes the same slot, so even systemIndex repeats. What does hold is that
every connection reported new is reported lost once. So each entry counts the connections
still open at its address - new events drained minus lost events drained - and goes when
that reaches zero.

Three defects came before that:

- HasNewIncomingConnection pushed a reused entry onto remoteClients a second time, and
  HasLostConnection then freed it twice.
- The old connection's lost event freed the entry the reconnect was using, and Receive
  dropped the live connection's input from then on.
- A lost event drained before its new one found no entry and freed nothing, and the new
  event then created an entry that nothing would free.

An address repeats only when the source port does, so the clients here are raw sockets bound
to fixed local ports. They close abortively - SO_LINGER with a zero timeout sends a RST -
which leaves no TIME_WAIT behind and lets a reconnect bind the same port again.
*/

using namespace RakNet;

namespace {

// TCP, so these share no space with the UDP ports the rest of the suite hardcodes, and
// distinct from the TCPInterface tests' ports so a stray listener is never ambiguous about
// which test left it. CreateListenSocket does not set SO_REUSEADDR before it binds, so no
// port is shared between cases.
constexpr unsigned short kReconnectListenPort = 61030;
constexpr unsigned short kReconnectClientPort = 61031;
constexpr unsigned short kLostFirstListenPort = 61032;
constexpr unsigned short kLostFirstClientPort = 61033;

// Loopback, so every wait here is over as soon as the threads have been scheduled once.
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

    // Connections TCPInterface has accepted, whether or not their new event was drained.
    unsigned short AcceptedCount() const { return tcpInterface->GetConnectionCount(); }
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

// A blocking TCP connection to the listener, made from the same local port every time.
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

} // namespace

TEST_CASE( "TelnetTransport keeps a client reconnecting from the same address", "[telnettransport][network]" )
{
    WinsockScope winsock;

    InspectableTelnetTransport telnet;
    REQUIRE( telnet.Start( kReconnectListenPort, true ) );

    SystemAddress firstAddress = UNASSIGNED_SYSTEM_ADDRESS;
    const __TCPSOCKET__ first = ConnectFromFixedPort( kReconnectListenPort, kReconnectClientPort );
    REQUIRE( WaitFor( [&] { return ( firstAddress = telnet.HasNewIncomingConnection() ) != UNASSIGNED_SYSTEM_ADDRESS; } ) );
    REQUIRE( telnet.ClientCount() == 1 );

    Abort( first );

    // The lost event for the first connection is queued, or soon will be; it is deliberately
    // not drained until the reconnect's new event has been.
    SystemAddress secondAddress = UNASSIGNED_SYSTEM_ADDRESS;
    const __TCPSOCKET__ second = ConnectFromFixedPort( kReconnectListenPort, kReconnectClientPort );
    REQUIRE( WaitFor( [&] { return ( secondAddress = telnet.HasNewIncomingConnection() ) != UNASSIGNED_SYSTEM_ADDRESS; } ) );
    REQUIRE( secondAddress == firstAddress );

    // The reused TelnetClient is already listed, so it is not listed again.
    CHECK( telnet.ClientCount() == 1 );

    // The first connection's lost event. It frees the TelnetClient once at most, and - the
    // second connection being open at the same address - not at all.
    REQUIRE( WaitFor( [&] { return telnet.HasLostConnection() == firstAddress; } ) );
    CHECK( telnet.ClientCount() == 1 );

    // The live connection's input still reassembles into a line.
    const char line[] = "hello\n";
    REQUIRE( send__( second, line, (int)strlen( line ), 0 ) == (int)strlen( line ) );
    Packet* received = 0;
    CHECK( WaitFor( [&] { return ( received = telnet.Receive() ) != 0; } ) );
    if( received )
    {
        CHECK( received->systemAddress == secondAddress );
        CHECK( strcmp( (const char*)received->data, "hello" ) == 0 );
        telnet.DeallocatePacket( received );
    }

    // The second connection's own lost event frees it.
    Abort( second );
    CHECK( WaitFor( [&] { return telnet.HasLostConnection() == secondAddress; } ) );
    CHECK( telnet.ClientCount() == 0 );

    telnet.Stop();
}

TEST_CASE( "TelnetTransport frees nothing twice and keeps nothing when a lost event comes first", "[telnettransport][network]" )
{
    WinsockScope winsock;

    InspectableTelnetTransport telnet;
    REQUIRE( telnet.Start( kLostFirstListenPort, true ) );

    SystemAddress address;
    REQUIRE( address.FromStringExplicitPort( "127.0.0.1", kLostFirstClientPort ) );

    // Accepted, but its new event left in the queue. Aborting before the accept could lose
    // the connection before TCPInterface ever reports it.
    const __TCPSOCKET__ client = ConnectFromFixedPort( kLostFirstListenPort, kLostFirstClientPort );
    REQUIRE( WaitFor( [&] { return telnet.AcceptedCount() == 1; } ) );
    Abort( client );

    // What ConsoleServer::Update does to a short-lived connection queued behind another: it
    // takes the lost event on one tick and the new event on a later one.
    REQUIRE( WaitFor( [&] { return telnet.HasLostConnection() == address; } ) );
    REQUIRE( telnet.HasNewIncomingConnection() == address );

    // Unfixed, the lost event found no entry and the new one created an entry nothing frees.
    CHECK( telnet.ClientCount() == 0 );

    telnet.Stop();
}
