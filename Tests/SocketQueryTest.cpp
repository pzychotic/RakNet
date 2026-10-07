#include "ConnectionWaits.h"
#include "GetTime.h"
#include "PeerScope.h"
#include "RakNetSocket2.h"
#include "RakNetStringMakers.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>
#include <vector>

/*
Pins the bound-socket getters to the list Startup publishes (ADR-0007, point 4). The sockets
don't change between Startup and Shutdown, so the getters never ask the network thread: they
answer as soon as Startup returns, without waiting for an update cycle, and the same way
under RAKPEER_USER_THREADED, where the test runs every update cycle itself.

The shapes pinned here:

- GetSockets is empty before Startup, holds every bound socket once Startup returns, and is
  empty again after Shutdown.
- GetSocket( UNASSIGNED_SYSTEM_ADDRESS ) is the first bound socket.
- GetSocket( a connected peer's address ) is the socket that connection uses, which need not
  be the first, and GetSocket( an unknown address ) is null.
- GetMyBoundAddress answers UNASSIGNED_SYSTEM_ADDRESS for an index outside the list.

RakPeerInterface functions explicitly tested:

    GetSocket
    GetSockets
    GetMyBoundAddress
*/

using namespace RakNet;

namespace {

// Ports no other test uses.
constexpr unsigned short kListFirstPort = 32300;
constexpr unsigned short kListSecondPort = 32301;
constexpr unsigned short kConnectionServerFirstPort = 32310;
constexpr unsigned short kConnectionServerSecondPort = 32311;
constexpr unsigned short kConnectionClientPort = 32312;
constexpr unsigned short kIndexFirstPort = 32320;
constexpr unsigned short kIndexSecondPort = 32321;

// Hang guard for the connection wait.
constexpr TimeMS kWaitBudgetMs = 10000;

/// Binds one socket of \a peer to 127.0.0.1 per port, in order.
StartupResult Start( RakPeerInterface* peer, std::vector<unsigned short> ports, unsigned int maxConnections = 1 )
{
    std::vector<SocketDescriptor> socketDescriptors;
    for( unsigned short port : ports )
        socketDescriptors.emplace_back( port, "127.0.0.1" );
    const StartupResult result = peer->Startup( maxConnections, socketDescriptors.data(), (unsigned)socketDescriptors.size() );
    peer->SetMaximumIncomingConnections( (unsigned short)maxConnections );
    return result;
}

SystemAddress Loopback( unsigned short port )
{
    SystemAddress address;
    address.FromStringExplicitPort( "127.0.0.1", port );
    return address;
}

std::vector<unsigned short> PortsOf( const std::vector<RakNetSocket2*>& sockets )
{
    std::vector<unsigned short> ports;
    for( RakNetSocket2* socket : sockets )
        ports.push_back( socket->GetBoundAddress().GetPort() );
    return ports;
}

} // namespace

TEST_CASE( "GetSockets holds every bound socket from the moment Startup returns until Shutdown", "[network]" )
{
    PeerScope peers;
    RakPeerInterface* peer = peers.Create();
    std::vector<RakNetSocket2*> sockets;

    peer->GetSockets( sockets );
    CHECK( sockets.empty() );
    CHECK( peer->GetSocket( UNASSIGNED_SYSTEM_ADDRESS ) == nullptr );

    REQUIRE( Start( peer, { kListFirstPort, kListSecondPort } ) == RAKNET_STARTED );

    // No update cycle has run under RAKPEER_USER_THREADED, and none is waited for otherwise.
    peer->GetSockets( sockets );
    CHECK( PortsOf( sockets ) == std::vector<unsigned short>{ kListFirstPort, kListSecondPort } );
    REQUIRE( sockets.size() == 2 );
    CHECK( peer->GetSocket( UNASSIGNED_SYSTEM_ADDRESS ) == sockets[0] );

    peer->Shutdown( 0 );

    peer->GetSockets( sockets );
    CHECK( sockets.empty() );
    CHECK( peer->GetSocket( UNASSIGNED_SYSTEM_ADDRESS ) == nullptr );
}

TEST_CASE( "GetSocket returns the socket a connection uses, and null for an address with no connection", "[network]" )
{
    PeerScope peers;
    RakPeerInterface* server = peers.Create();
    RakPeerInterface* client = peers.Create();
    REQUIRE( Start( server, { kConnectionServerFirstPort, kConnectionServerSecondPort } ) == RAKNET_STARTED );
    REQUIRE( Start( client, { kConnectionClientPort } ) == RAKNET_STARTED );

    // To the server's second socket, so its answer can't be the first bound socket by chance.
    const SystemAddress serverAddress = Loopback( kConnectionServerSecondPort );
    const SystemAddress clientAddress = Loopback( kConnectionClientPort );
    REQUIRE( client->Connect( "127.0.0.1", kConnectionServerSecondPort, nullptr, 0 ) == CONNECTION_ATTEMPT_STARTED );

    // Not ConnectionWaits::ConnectAndWait, which connects to the first socket and runs no
    // update cycle: under RAKPEER_USER_THREADED the wait has to run both peers' update cycles itself.
    const TimeMS deadline = GetTimeMS() + kWaitBudgetMs;
    while( client->GetConnectionState( serverAddress ) != IS_CONNECTED || server->GetConnectionState( clientAddress ) != IS_CONNECTED )
    {
        REQUIRE( ConnectionWaits::Expired( deadline ) == false );
        ConnectionWaits::RunUpdateCycle( server );
        ConnectionWaits::Drain( server );
        ConnectionWaits::RunUpdateCycle( client );
        ConnectionWaits::Drain( client );
        std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
    }

    std::vector<RakNetSocket2*> serverSockets;
    server->GetSockets( serverSockets );
    REQUIRE( serverSockets.size() == 2 );
    CHECK( server->GetSocket( clientAddress ) == serverSockets[1] );

    std::vector<RakNetSocket2*> clientSockets;
    client->GetSockets( clientSockets );
    REQUIRE( clientSockets.size() == 1 );
    CHECK( client->GetSocket( serverAddress ) == clientSockets[0] );

    // The server's first socket has no connection at all.
    CHECK( client->GetSocket( Loopback( kConnectionServerFirstPort ) ) == nullptr );
    CHECK( server->GetSocket( Loopback( 1 ) ) == nullptr );
}

TEST_CASE( "GetMyBoundAddress answers UNASSIGNED_SYSTEM_ADDRESS for an index outside the bound sockets", "[network]" )
{
    PeerScope peers;
    RakPeerInterface* peer = peers.Create();
    CHECK( peer->GetMyBoundAddress( 0 ) == UNASSIGNED_SYSTEM_ADDRESS );

    REQUIRE( Start( peer, { kIndexFirstPort, kIndexSecondPort } ) == RAKNET_STARTED );

    CHECK( peer->GetMyBoundAddress( 0 ).GetPort() == kIndexFirstPort );
    CHECK( peer->GetMyBoundAddress( 1 ).GetPort() == kIndexSecondPort );
    CHECK( peer->GetMyBoundAddress( -1 ) == UNASSIGNED_SYSTEM_ADDRESS );
    CHECK( peer->GetMyBoundAddress( 2 ) == UNASSIGNED_SYSTEM_ADDRESS );
    CHECK( peer->GetMyBoundAddress( 1000 ) == UNASSIGNED_SYSTEM_ADDRESS );
}
