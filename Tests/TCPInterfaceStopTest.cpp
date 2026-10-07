#include "LoopbackTCP.h"
#include "TCPInterface.h"
#include "RakNetTypes.h"
#include "SocketDefines.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

/*
Pins that TCPInterface::Stop closes only the descriptors the interface still owns.

A blocking Connect hands its socket to Stop for the length of the connect, so that Stop can
abort it. The defect was that a connect that succeeded never took it back: the descriptor
stayed in Stop's list after the connection owned it, and Stop closed that number again long
after CloseConnection had. POSIX and Winsock both hand a freed number out again, so by then
it is usually somebody else's socket, and Stop closed it from under them.

The test makes that somebody the test itself: it opens sockets right after CloseConnection,
so one of them takes the freed number, and requires all of them to be open after Stop.
*/

using namespace LoopbackTCP;
using namespace RakNet;

namespace {

// TCP, so it shares no space with the UDP ports the rest of the suite hardcodes, and
// distinct from the other TCPInterface tests' ports so a stray listener is never ambiguous
// about which test left it.
constexpr unsigned short kListenPort = 31014;

// More than the descriptors that can be freed between CloseConnection and the opens below:
// the client's connection, and the server's end of it if the server has noticed the close.
constexpr int kBystanderCount = 4;

} // namespace

TEST_CASE( "TCPInterface Stop leaves alone a descriptor a closed blocking connection used to have", "[tcpinterface][network]" )
{
    TCPInterface server;
    REQUIRE( server.Start( kListenPort, 4 ) );

    TCPInterface client;
    REQUIRE( client.Start( 0, 0, 1 ) );

    const SystemAddress address = client.Connect( "127.0.0.1", kListenPort, true, AF_INET );
    REQUIRE( address != UNASSIGNED_SYSTEM_ADDRESS );
    REQUIRE( WaitFor( [&] { return server.GetConnectionCount() == 1; } ) );

    // Closes the connection's descriptor now, on this thread, so the opens below are the
    // next to hand out numbers.
    REQUIRE( client.CloseConnection( address ) );

    std::vector<__TCPSOCKET__> bystanders;
    for( int i = 0; i < kBystanderCount; i++ )
    {
        const __TCPSOCKET__ bystander = socket__( AF_INET, SOCK_STREAM, 0 );
        REQUIRE( IsSocket( bystander ) );
        bystanders.push_back( bystander );
    }

    client.Stop();

    for( __TCPSOCKET__ bystander : bystanders )
        CHECK( IsSocket( bystander ) );

    for( __TCPSOCKET__ bystander : bystanders )
        closesocket__( bystander );
    server.Stop();
}
