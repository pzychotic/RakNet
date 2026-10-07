#include "BitStream.h"
#include "ConnectionWaits.h"
#include "GetTime.h"
#include "MessageIdentifiers.h"
#include "RakPeer.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

/*
Pins the published view that RakPeer's network thread copies out of its connection records
for the user thread (ADR-0007, point 4).

The shapes pinned here:

- Once a connection is up, a lookup by address, by RakNetGUID and by index into the
  connection records each finds the other Peer, connected, one full update cycle later.
- Once it is closed, all three miss one full update cycle later.
- Shutdown empties the view.
- Receive shifts a timestamp from the user thread without a network-thread lookup.

"One full update cycle later" is counted, not timed: a user-update callback counts the
cycles, and a cycle that starts after the state was observed has published it by the time
the next one starts.

RakPeerInterface functions explicitly tested:

    None. The view's lookups are protected in RakPeer, and ViewedPeer below exposes them.
    Receive is exercised for the timestamp case.
*/

using namespace RakNet;

namespace
{

// Hang guard for every wait below, not a settle time: each normally ends within a few
// hundred milliseconds.
constexpr TimeMS kWaitBudgetMs = 10000;

/// A RakPeer that shows its published view, which is protected in RakPeer, and counts its
/// update cycles.
///
/// Built directly rather than through PeerScope, which hands out RakPeerInterface only.
class ViewedPeer : public RakPeer
{
public:
    using RakPeer::PublishedRemoteSystem;

    ~ViewedPeer() override
    {
        // The network thread calls CountCycle, so it has to be gone before this part of
        // the object is. ~RakPeer's own Shutdown is then a no-op.
        Shutdown( 0 );
    }

    void Start( unsigned short port )
    {
        SocketDescriptor socketDescriptor( port, "127.0.0.1" );
        REQUIRE( Startup( 1, &socketDescriptor, 1 ) == RAKNET_STARTED );
        SetMaximumIncomingConnections( 1 );
        SetUserUpdateThread( &ViewedPeer::CountCycle, this );
    }

    SystemAddress Address()
    {
        SystemAddress address = GetMyBoundAddress();
        address.FromStringExplicitPort( "127.0.0.1", address.GetPort() );
        return address;
    }

    /// Blocks until an update cycle that started after this call has finished, so it has
    /// published whatever the connection records held at the call. The callback runs at the
    /// start of each cycle, so the second one counted after the call ends the first cycle
    /// that started after it.
    void WaitForAFullCycle()
    {
        const unsigned long long target = cycles.load() + 2;
        const TimeMS deadline = GetTimeMS() + kWaitBudgetMs;
        while( cycles.load() < target )
        {
            REQUIRE_FALSE( ConnectionWaits::Expired( deadline ) );
            std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
        }
    }

    using RakPeer::GetPublishedByAddress;
    using RakPeer::GetPublishedByGuid;
    using RakPeer::GetPublishedByIndex;

private:
    static void CountCycle( RakPeerInterface*, void* data )
    {
        ++static_cast<ViewedPeer*>( data )->cycles;
    }

    std::atomic<unsigned long long> cycles{ 0 };
};

/// Checks that \a peer's view finds \a other by address, by RakNetGUID and at \a index,
/// connected, and that all three lookups agree.
void CheckViewFinds( const ViewedPeer& peer, const SystemAddress& otherAddress, const RakNetGUID& otherGuid, unsigned int index )
{
    ViewedPeer::PublishedRemoteSystem byAddress{};
    ViewedPeer::PublishedRemoteSystem byGuid{};
    ViewedPeer::PublishedRemoteSystem byIndex{};

    REQUIRE( peer.GetPublishedByAddress( otherAddress, byAddress ) );
    REQUIRE( peer.GetPublishedByGuid( otherGuid, byGuid ) );
    REQUIRE( peer.GetPublishedByIndex( index, byIndex ) );

    for( const ViewedPeer::PublishedRemoteSystem* entry : { &byAddress, &byGuid, &byIndex } )
    {
        CHECK( entry->index == index );
        CHECK( entry->systemAddress == otherAddress );
        CHECK( entry->guid == otherGuid );
        CHECK( entry->connectMode == RakPeer::RemoteSystemStruct::CONNECTED );
    }
}

/// Checks that none of the three lookups finds \a other in \a peer's view.
void CheckViewMisses( const ViewedPeer& peer, const SystemAddress& otherAddress, const RakNetGUID& otherGuid, unsigned int index )
{
    ViewedPeer::PublishedRemoteSystem entry{};
    CHECK_FALSE( peer.GetPublishedByAddress( otherAddress, entry ) );
    CHECK_FALSE( peer.GetPublishedByGuid( otherGuid, entry ) );
    CHECK_FALSE( peer.GetPublishedByIndex( index, entry ) );
}

} // namespace

TEST_CASE( "The published view finds a connected System by address, RakNetGUID and index, and loses it once the connection closes", "[network]" )
{
    ViewedPeer server;
    server.Start( 0 );
    ViewedPeer client;
    client.Start( 0 );

    const SystemAddress serverAddress = server.Address();
    const SystemAddress clientAddress = client.Address();

    ConnectionWaits::ConnectAndWait( &client, &server );

    const int serverIndex = client.GetIndexFromSystemAddress( serverAddress );
    const int clientIndex = server.GetIndexFromSystemAddress( clientAddress );
    REQUIRE( serverIndex >= 0 );
    REQUIRE( clientIndex >= 0 );

    client.WaitForAFullCycle();
    server.WaitForAFullCycle();

    {
        INFO( "the client's view of the server" );
        CheckViewFinds( client, serverAddress, server.GetMyGUID(), (unsigned int)serverIndex );
    }
    {
        INFO( "the server's view of the client" );
        CheckViewFinds( server, clientAddress, client.GetMyGUID(), (unsigned int)clientIndex );
    }

    client.CloseConnection( serverAddress, true, 0, LOW_PRIORITY );
    RakPeerInterface* const both[] = { &client, &server };
    REQUIRE( ConnectionWaits::DrainUntil(
        both, 2,
        [&] {
            return client.GetConnectionState( serverAddress ) != IS_CONNECTED &&
                   client.GetConnectionState( serverAddress ) != IS_DISCONNECTING &&
                   server.GetConnectionState( clientAddress ) != IS_CONNECTED &&
                   server.GetConnectionState( clientAddress ) != IS_DISCONNECTING;
        },
        kWaitBudgetMs ) );

    client.WaitForAFullCycle();
    server.WaitForAFullCycle();

    {
        INFO( "the client's view of the server" );
        CheckViewMisses( client, serverAddress, server.GetMyGUID(), (unsigned int)serverIndex );
    }
    {
        INFO( "the server's view of the client" );
        CheckViewMisses( server, clientAddress, client.GetMyGUID(), (unsigned int)clientIndex );
    }
}

TEST_CASE( "Shutdown empties the published view", "[network]" )
{
    ViewedPeer server;
    server.Start( 0 );
    ViewedPeer client;
    client.Start( 0 );

    const SystemAddress serverAddress = server.Address();

    ConnectionWaits::ConnectAndWait( &client, &server );

    const int serverIndex = client.GetIndexFromSystemAddress( serverAddress );
    REQUIRE( serverIndex >= 0 );

    client.WaitForAFullCycle();
    CheckViewFinds( client, serverAddress, server.GetMyGUID(), (unsigned int)serverIndex );

    client.Shutdown( 0 );

    CheckViewMisses( client, serverAddress, server.GetMyGUID(), (unsigned int)serverIndex );
}

TEST_CASE( "Receive delivers a timestamped Message without going to the connection records' hash chain", "[network]" )
{
    // Receive runs on the user's thread and shifts every ID_TIMESTAMP by the sender's
    // clock differential. That lookup must not be one that asserts it is inside
    // RunUpdateCycle, or every Debug build that receives a timestamp aborts.
    ViewedPeer server;
    server.Start( 0 );
    ViewedPeer client;
    client.Start( 0 );

    ConnectionWaits::ConnectAndWait( &client, &server );

    BitStream message;
    message.Write( (MessageID)ID_TIMESTAMP );
    message.Write( RakNet::GetTime() );
    message.Write( (MessageID)ID_USER_PACKET_ENUM );
    REQUIRE( client.Send( &message, HIGH_PRIORITY, RELIABLE_ORDERED, 0, server.Address(), false ) != 0 );

    bool received = false;
    const TimeMS deadline = GetTimeMS() + kWaitBudgetMs;
    while( received == false && ConnectionWaits::Expired( deadline ) == false )
    {
        for( Packet* packet = server.Receive(); packet != 0; packet = server.Receive() )
        {
            if( packet->data[0] == ID_TIMESTAMP )
                received = true;
            server.DeallocatePacket( packet );
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
    }
    CHECK( received );
}
