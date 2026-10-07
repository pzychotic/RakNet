#if !defined( _WIN32 )

#include "LoopbackTCP.h"
#include "TCPInterface.h"
#include "RakNetTypes.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>

#include <fcntl.h>
#include <unistd.h>

/*
Pins that TCPInterface uses a socket that the operating system numbers 0.

POSIX hands out the lowest free descriptor, and 0 is free in any process that starts with
stdin closed, such as a daemon. TCPInterface used 0 as its "no socket" value, so a listen
socket numbered 0 was never accepted on, and a connection numbered 0 was reported as a
failed Connect.

Each case closes fd 0 for its duration, so that the socket under test is the one to get it,
and requires that fd 0 is a socket before it requires that data flows. POSIX only: Winsock
never hands out 0.
*/

using namespace LoopbackTCP;
using namespace RakNet;

namespace {

// TCP, so these share no space with the UDP ports the rest of the suite hardcodes, and
// distinct from the other TCPInterface tests' ports so a stray listener is never ambiguous
// about which test left it. One per case, since the listen socket gets no SO_REUSEADDR.
constexpr unsigned short kListenOnZeroPort = 31015;
constexpr unsigned short kConnectOnZeroPort = 31016;

// Closes fd 0 and puts it back on scope exit, so a failed assertion doesn't leave the
// process without stdin.
class StdinClosed
{
public:
    StdinClosed()
    : saved( dup( 0 ) )
    {
        close( 0 );
    }
    ~StdinClosed()
    {
        if( saved == -1 )
            return;
        dup2( saved, 0 );
        close( saved );
    }

    StdinClosed( const StdinClosed& ) = delete;
    StdinClosed& operator=( const StdinClosed& ) = delete;

private:
    int saved;
};

// The first packet the interface receives within the wait, as a string; empty if none.
std::string ReceiveString( TCPInterface& tcpInterface )
{
    std::string received;
    WaitFor( [&] {
        Packet* packet = tcpInterface.Receive();
        if( packet == 0 )
            return false;
        received.assign( (const char*)packet->data, packet->length );
        tcpInterface.DeallocatePacket( packet );
        return true;
    } );
    return received;
}

void Send( TCPInterface& tcpInterface, const char* text, const SystemAddress& address, bool broadcast = false )
{
    tcpInterface.Send( text, (unsigned int)strlen( text ), address, broadcast );
}

} // namespace

TEST_CASE( "TCPInterface accepts on a listen socket numbered 0", "[tcpinterface][network]" )
{
    // Declared first, so fd 0 is put back only after both interfaces have closed theirs.
    StdinClosed stdinClosed;

    TCPInterface server;
    REQUIRE( server.Start( kListenOnZeroPort, 4 ) );
    REQUIRE( IsSocket( 0 ) );

    TCPInterface client;
    REQUIRE( client.Start( 0, 0, 1 ) );

    const SystemAddress serverAddress = client.Connect( "127.0.0.1", kListenOnZeroPort, true, AF_INET );
    REQUIRE( serverAddress != UNASSIGNED_SYSTEM_ADDRESS );

    Send( client, "ping", serverAddress );
    CHECK( ReceiveString( server ) == "ping" );

    client.Stop();
    server.Stop();
}

TEST_CASE( "TCPInterface connects on a socket numbered 0", "[tcpinterface][network]" )
{
    // Declared first, so fd 0 is put back only after both interfaces have closed theirs.
    StdinClosed stdinClosed;

    // Holds fd 0 while the server's listen socket is made, so that the connection is the
    // one to get it.
    const int placeholder = open( "/dev/null", O_RDONLY );
    REQUIRE( placeholder == 0 );

    TCPInterface server;
    const bool isServerStarted = server.Start( kConnectOnZeroPort, 4 );
    close( placeholder );
    REQUIRE( isServerStarted );

    TCPInterface client;
    REQUIRE( client.Start( 0, 0, 1 ) );

    const SystemAddress serverAddress = client.Connect( "127.0.0.1", kConnectOnZeroPort, true, AF_INET );
    REQUIRE( IsSocket( 0 ) );
    REQUIRE( serverAddress != UNASSIGNED_SYSTEM_ADDRESS );

    Send( client, "ping", serverAddress );
    REQUIRE( ReceiveString( server ) == "ping" );

    // The reply comes back over the client's socket 0 the other way.
    Send( server, "pong", UNASSIGNED_SYSTEM_ADDRESS, true );
    CHECK( ReceiveString( client ) == "pong" );

    client.Stop();
    server.Stop();
}

#endif
