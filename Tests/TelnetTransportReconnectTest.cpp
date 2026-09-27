#include "Plugins/TelnetTransport.h"
#include "RakNetTypes.h"
#include "SocketDefines.h"
#include "SocketIncludes.h"
#include "WSAStartupSingleton.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstring>
#include <thread>

/*
Pins that a telnet client reconnecting from the address it last connected from is listed
once, and freed once.

TelnetTransport keeps a TelnetClient per remote address for line reassembly.
HasNewIncomingConnection looked for an existing entry with the new connection's address and
reused it - and then pushed the pointer onto remoteClients unconditionally, so a reused client
went in a second time. HasLostConnection deletes every entry whose address matches, which then
deleted that one object twice. Upstream's swap-remove loop skipped the second copy and left it
dangling instead; the fork's erase loop visits both, so it was a double free on the spot.

An address repeats only when the source port does, so the client here is a raw socket bound
to a fixed local port. It closes abortively - SO_LINGER with a zero timeout sends a RST - which
leaves no TIME_WAIT behind and lets the reconnect bind the same port again. Both connections'
new events are drained before either lost event: that is the ordering that puts one client in
the list twice, and it is ordinary for any application that polls the three queues at its own
pace.

Unfixed, the entry count after the second connection is 2, and the first lost event frees the
same TelnetClient twice, which the MSVC debug heap or ASan stops right there.
*/

using namespace RakNet;

namespace {

// TCP, so these share no space with the UDP ports the rest of the suite hardcodes, and
// distinct from the TCPInterface tests' ports so a stray listener is never ambiguous about
// which test left it.
constexpr unsigned short kListenPort = 61030;
constexpr unsigned short kClientPort = 61031;

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

// remoteClients is protected; the count is the only thing the test needs from it.
class InspectableTelnetTransport : public TelnetTransport
{
public:
    size_t ClientCount() const { return remoteClients.size(); }
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

// A blocking TCP connection to the listener, made from kClientPort every time.
__TCPSOCKET__ ConnectFromFixedPort()
{
    const __TCPSOCKET__ s = socket__( AF_INET, SOCK_STREAM, IPPROTO_TCP );
    REQUIRE( s != (__TCPSOCKET__)-1 );

    const int reuse = 1;
    REQUIRE( setsockopt__( s, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof( reuse ) ) == 0 );

    const sockaddr_in local = LoopbackAddress( kClientPort );
    REQUIRE( bind__( s, (const sockaddr*)&local, sizeof( local ) ) == 0 );

    const sockaddr_in remote = LoopbackAddress( kListenPort );
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

TEST_CASE( "TelnetTransport lists a client reconnecting from the same address once", "[telnettransport][network]" )
{
    WinsockScope winsock;

    InspectableTelnetTransport telnet;
    REQUIRE( telnet.Start( kListenPort, true ) );

    SystemAddress firstAddress = UNASSIGNED_SYSTEM_ADDRESS;
    const __TCPSOCKET__ first = ConnectFromFixedPort();
    REQUIRE( WaitFor( [&] { return ( firstAddress = telnet.HasNewIncomingConnection() ) != UNASSIGNED_SYSTEM_ADDRESS; } ) );
    REQUIRE( telnet.ClientCount() == 1 );

    Abort( first );

    // The lost event for the first connection is queued, or soon will be; it is deliberately
    // not drained until the reconnect's new event has been.
    SystemAddress secondAddress = UNASSIGNED_SYSTEM_ADDRESS;
    const __TCPSOCKET__ second = ConnectFromFixedPort();
    REQUIRE( WaitFor( [&] { return ( secondAddress = telnet.HasNewIncomingConnection() ) != UNASSIGNED_SYSTEM_ADDRESS; } ) );
    REQUIRE( secondAddress == firstAddress );

    // The reused TelnetClient is already listed. Unfixed, it is listed twice.
    CHECK( telnet.ClientCount() == 1 );

    // The load-bearing call. Unfixed, it deletes the same TelnetClient twice.
    //
    // Zero, not one, even though the second connection is still open: the first connection's
    // lost event matches by address, so it frees the client the reconnect is reusing, and
    // the live connection's input is dropped from here on. That is the behaviour upstream
    // has too, and it is not what this test is about; what it pins is that nothing
    // dangling stays listed. A fix that tells the two connections apart changes this line.
    REQUIRE( WaitFor( [&] { return telnet.HasLostConnection() == firstAddress; } ) );
    CHECK( telnet.ClientCount() == 0 );

    // The second connection's own lost event finds nothing left to free.
    Abort( second );
    CHECK( WaitFor( [&] { return telnet.HasLostConnection() == secondAddress; } ) );
    CHECK( telnet.ClientCount() == 0 );

    telnet.Stop();
}
