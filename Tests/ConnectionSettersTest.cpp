#include "ConnectionWaits.h"
#include "GetTime.h"
#include "MessageIdentifiers.h"
#include "RakNetStringMakers.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>
#include <vector>

/*
Pins the setters that change a connection record, which the user thread hands to the
network thread as buffered commands (ADR-0007, points 2 and 4).

The shapes pinned here:

- SetTimeoutTime on one connection reaches that connection's record within a few update
  cycles, and leaves the Peer-wide default and the other connections alone.
- SetTimeoutTime on every connection updates the default at once, and reaches each open
  connection within a few update cycles.
- A split-message progress interval set before Connect reaches the new connection, and
  one set while the connection is open reaches it too.
- ApplyNetworkSimulator called while a connection is open delays that connection's
  datagrams. The simulator only runs in Debug builds.

RakPeerInterface functions explicitly tested:

    SetTimeoutTime
    GetTimeoutTime
    SetSplitMessageProgressInterval
    GetSplitMessageProgressInterval
    ApplyNetworkSimulator
    IsNetworkSimulatorActive
*/

using namespace RakNet;

namespace {

// Ports no other test uses.
constexpr unsigned short kTimeoutServerPort = 32180;
constexpr unsigned short kTimeoutClientBasePort = 32181;
constexpr unsigned short kProgressServerPort = 32190;
constexpr unsigned short kProgressClientPort = 32191;
constexpr unsigned short kSimulatorServerPort = 32200;
constexpr unsigned short kSimulatorClientPort = 32201;

// Hang guard for every wait below, not a settle time: each normally ends within a few
// hundred milliseconds.
constexpr TimeMS kWaitBudgetMs = 10000;

// Timeouts no build uses as its default, so a match can only come from the setter.
constexpr TimeMS kOneConnectionTimeout = 23456;
constexpr TimeMS kEveryConnectionTimeout = 34567;

// Several MTUs, so it arrives split and reports progress on every chunk.
constexpr unsigned int kSplitMessageSize = 20000;

// The extra delay the simulator adds to each of the client's datagrams.
constexpr unsigned short kSimulatedPingMs = 150;

/// A Peer bound to 127.0.0.1 on a fixed port, shut down and destroyed with the scope.
class BoundPeer
{
public:
    BoundPeer( unsigned short port, unsigned int maxConnections )
    : peer( RakPeerInterface::GetInstance() )
    {
        SocketDescriptor socketDescriptor( port, "127.0.0.1" );
        REQUIRE( peer->Startup( maxConnections, &socketDescriptor, 1 ) == RAKNET_STARTED );
        peer->SetMaximumIncomingConnections( (unsigned short)maxConnections );
        address.FromStringExplicitPort( "127.0.0.1", port );
    }

    ~BoundPeer() { RakPeerInterface::DestroyInstance( peer ); }

    BoundPeer( const BoundPeer& ) = delete;
    BoundPeer& operator=( const BoundPeer& ) = delete;

    RakPeerInterface* operator->() const { return peer; }
    RakPeerInterface* Get() const { return peer; }
    const SystemAddress& Address() const { return address; }

private:
    RakPeerInterface* peer;
    SystemAddress address;
};

/// Receives on \a peer until a Message with \a id arrives, or the budget is spent.
bool WaitForMessage( RakPeerInterface* peer, unsigned char id )
{
    const TimeMS deadline = GetTimeMS() + kWaitBudgetMs;
    while( ConnectionWaits::Expired( deadline ) == false )
    {
        for( Packet* packet = peer->Receive(); packet != nullptr; packet = peer->Receive() )
        {
            const bool found = packet->length > 0 && packet->data[0] == id;
            peer->DeallocatePacket( packet );
            if( found )
                return true;
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
    }
    return false;
}

/// Sends a message large enough to be split from \a client to every connection.
void SendSplitMessage( BoundPeer& client )
{
    std::vector<char> message( kSplitMessageSize, 0 );
    message[0] = (char)( ID_USER_PACKET_ENUM + 1 );
    REQUIRE( client->Send( message.data(), (int)message.size(), HIGH_PRIORITY, RELIABLE_ORDERED, 0, UNASSIGNED_SYSTEM_ADDRESS, true ) > 0 );
}

} // namespace

TEST_CASE( "SetTimeoutTime reaches an open connection a few cycles later", "[network]" )
{
    BoundPeer server( kTimeoutServerPort, 2 );
    BoundPeer first( kTimeoutClientBasePort, 1 );
    BoundPeer second( (unsigned short)( kTimeoutClientBasePort + 1 ), 1 );
    ConnectionWaits::ConnectAndWait( first.Get(), server.Get() );
    ConnectionWaits::ConnectAndWait( second.Get(), server.Get() );

    const TimeMS defaultTimeout = server->GetTimeoutTime( UNASSIGNED_SYSTEM_ADDRESS );
    REQUIRE( defaultTimeout != kOneConnectionTimeout );
    REQUIRE( server->GetTimeoutTime( first.Address() ) == defaultTimeout );
    RakPeerInterface* const serverOnly[] = { server.Get() };

    SECTION( "on one connection" )
    {
        server->SetTimeoutTime( kOneConnectionTimeout, first.Address() );
        CHECK( ConnectionWaits::DrainUntil(
            serverOnly, 1, [&] { return server->GetTimeoutTime( first.Address() ) == kOneConnectionTimeout; }, kWaitBudgetMs ) );
        CHECK( server->GetTimeoutTime( UNASSIGNED_SYSTEM_ADDRESS ) == defaultTimeout );
        CHECK( server->GetTimeoutTime( second.Address() ) == defaultTimeout );
    }

    SECTION( "on every connection" )
    {
        server->SetTimeoutTime( kEveryConnectionTimeout, UNASSIGNED_SYSTEM_ADDRESS );
        // The default is the setter's own, so it changes straight away.
        CHECK( server->GetTimeoutTime( UNASSIGNED_SYSTEM_ADDRESS ) == kEveryConnectionTimeout );
        CHECK( ConnectionWaits::DrainUntil(
            serverOnly, 1,
            [&] {
                return server->GetTimeoutTime( first.Address() ) == kEveryConnectionTimeout &&
                       server->GetTimeoutTime( second.Address() ) == kEveryConnectionTimeout;
            },
            kWaitBudgetMs ) );
    }

    SECTION( "on one connection, then on every connection" )
    {
        // Applied in the order they were called, so the second one wins.
        server->SetTimeoutTime( kOneConnectionTimeout, first.Address() );
        server->SetTimeoutTime( kEveryConnectionTimeout, UNASSIGNED_SYSTEM_ADDRESS );
        CHECK( ConnectionWaits::DrainUntil(
            serverOnly, 1,
            [&] {
                return server->GetTimeoutTime( first.Address() ) == kEveryConnectionTimeout &&
                       server->GetTimeoutTime( second.Address() ) == kEveryConnectionTimeout;
            },
            kWaitBudgetMs ) );
    }
}

TEST_CASE( "A split-message progress interval reaches connections opened before and after it was set", "[network]" )
{
    BoundPeer server( kProgressServerPort, 1 );
    BoundPeer client( kProgressClientPort, 1 );

    SECTION( "set before Connect" )
    {
        server->SetSplitMessageProgressInterval( 1 );
        CHECK( server->GetSplitMessageProgressInterval() == 1 );
        ConnectionWaits::ConnectAndWait( client.Get(), server.Get() );
    }

    SECTION( "set while the connection is open" )
    {
        ConnectionWaits::ConnectAndWait( client.Get(), server.Get() );
        server->SetSplitMessageProgressInterval( 1 );
        CHECK( server->GetSplitMessageProgressInterval() == 1 );
        // The setter only queues a command, and nothing the user thread can read shows when
        // the network thread has applied it. Ten update cycles is ample.
        std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
    }

    SendSplitMessage( client );
    CHECK( WaitForMessage( server.Get(), ID_DOWNLOAD_PROGRESS ) );
}

TEST_CASE( "ApplyNetworkSimulator reaches an open connection", "[network]" )
{
    BoundPeer server( kSimulatorServerPort, 1 );
    BoundPeer client( kSimulatorClientPort, 1 );
    ConnectionWaits::ConnectAndWait( client.Get(), server.Get() );

    client->ApplyNetworkSimulator( 0.0f, kSimulatedPingMs, 0 );
    if( client->IsNetworkSimulatorActive() == false )
        SKIP( "The network simulator only runs in Debug builds" );
#ifdef FLIP_SEND_ORDER_TEST
    SKIP( "FLIP_SEND_ORDER_TEST replaces the simulated delay with a reversed send order" );
#endif

    // Pongs from before the simulator took effect may still arrive, so wait for a ping
    // that shows the delay rather than reading one.
    RakPeerInterface* const both[] = { client.Get(), server.Get() };
    CHECK( ConnectionWaits::DrainUntil(
        both, 2,
        [&] {
            server->Ping( client.Address() );
            return server->GetLastPing( client.Address() ) >= kSimulatedPingMs;
        },
        kWaitBudgetMs ) );
}
