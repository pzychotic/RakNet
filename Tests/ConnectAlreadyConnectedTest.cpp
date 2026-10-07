#include "ConnectionWaits.h"
#include "MessageIdentifiers.h"
#include "RakNetStringMakers.h"
#include "RakPeer.h"
#include "RakPeerInterface.h"
#include "RakNetSocket2.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

/*
Pins Connect's "already connected" answer, which the user thread takes from the published
view rather than the connection records (ADR-0007, point 5).

The shapes pinned here:

- Once the view shows a connection to B, Connect and ConnectWithSocket toward B return
  ALREADY_CONNECTED_TO_ENDPOINT and queue nothing.
- Called while A's network thread has opened the connection record for B but not yet
  published it, Connect accepts the request. The network threads turn the redundant attempt
  down against their connection records, so A still ends up with exactly one connection to
  B, and Receive shows ID_CONNECTION_REQUEST_ACCEPTED once.

RakPeerInterface functions explicitly tested:

    Connect
    ConnectWithSocket
*/

using namespace RakNet;

namespace {

// Ports no other test uses.
constexpr unsigned short kConnectedServerPort = 32160;
constexpr unsigned short kConnectedClientPort = 32161;
constexpr unsigned short kStaleServerPort = 32170;
constexpr unsigned short kStaleClientPort = 32171;

// Hang guard for every wait below, not a settle time: each normally ends within a few
// hundred milliseconds.
constexpr TimeMS kWaitBudgetMs = 10000;

// The second attempt's lifetime: short, so the test waits little for it to run out.
constexpr unsigned kAttemptCount = 4;
constexpr unsigned kAttemptIntervalMs = 100;

// Past the second attempt's last send, plus room for B's answer to arrive and reach
// Receive. A settle time, not a hang guard: the test receives for all of it.
constexpr TimeMS kSecondAttemptSettleMs = kAttemptCount * kAttemptIntervalMs + 500;

/// A RakPeer bound to 127.0.0.1 that can stop its network thread in the window between
/// opening a connection record and publishing it, which is protected in RakPeer. Shut
/// down before it is destroyed.
///
/// Built directly rather than through PeerScope, which hands out RakPeerInterface only.
class PausingPeer : public RakPeer
{
public:
    ~PausingPeer() override
    {
        Release();
        // The network thread calls the overrides below, so it has to be gone before this
        // part of the object is. ~RakPeer's own Shutdown is then a no-op.
        Shutdown( 0 );
    }

    void Start( unsigned short port )
    {
        SocketDescriptor socketDescriptor( port, "127.0.0.1" );
        REQUIRE( Startup( 1, &socketDescriptor, 1 ) == RAKNET_STARTED );
        SetMaximumIncomingConnections( 1 );
        SetUserUpdateThread( &PausingPeer::NoteNetworkThread, this );
        address.FromStringExplicitPort( "127.0.0.1", port );
    }

    const SystemAddress& Address() const { return address; }

    /// The next time the network thread finishes a datagram with a connection record open
    /// for \a target that the view does not show yet, it stops there until Release.
    void PauseWhenUnpublished( const SystemAddress& target )
    {
        std::lock_guard<std::mutex> guard( pauseMutex );
        pauseTarget = target;
        armed = true;
    }

    /// Blocks until the network thread has stopped. False at the budget.
    bool WaitUntilPaused()
    {
        std::unique_lock<std::mutex> lock( pauseMutex );
        return pauseChanged.wait_for( lock, std::chrono::milliseconds( kWaitBudgetMs ), [this] { return paused; } );
    }

    void Release()
    {
        std::lock_guard<std::mutex> guard( pauseMutex );
        armed = false;
        released = true;
        pauseChanged.notify_all();
    }

    /// Whether a connection attempt toward \a target is still queued.
    bool AttemptQueued( const SystemAddress& target )
    {
        std::lock_guard<std::mutex> guard( requestedConnectionQueueMutex );
        for( const RequestedConnectionStruct* request : requestedConnectionQueue )
        {
            if( request->systemAddress == target )
                return true;
        }
        return false;
    }

    void DeallocRNS2RecvStruct( RNS2RecvStruct* s, const char* file, unsigned int line ) override
    {
        RakPeer::DeallocRNS2RecvStruct( s, file, line );

        // The receive thread deallocates too. Only the network thread may read the records.
        if( std::this_thread::get_id() != networkThread.load() )
            return;

        std::unique_lock<std::mutex> lock( pauseMutex );
        if( armed == false || RecordOpenFor( pauseTarget ) == false )
            return;
        PublishedRemoteSystem entry;
        if( GetPublishedByAddress( pauseTarget, entry ) )
            return;

        armed = false;
        paused = true;
        pauseChanged.notify_all();
        pauseChanged.wait( lock, [this] { return released; } );
    }

private:
    static void NoteNetworkThread( RakPeerInterface*, void* data )
    {
        static_cast<PausingPeer*>( data )->networkThread = std::this_thread::get_id();
    }

    /// Network thread only.
    bool RecordOpenFor( const SystemAddress& target ) const
    {
        for( unsigned int i = 0; i < GetMaximumNumberOfPeers(); i++ )
        {
            if( remoteSystemList[i].isActive && remoteSystemList[i].systemAddress == target )
                return true;
        }
        return false;
    }

    SystemAddress address;
    std::atomic<std::thread::id> networkThread{};

    std::mutex pauseMutex;
    std::condition_variable pauseChanged;
    SystemAddress pauseTarget;
    bool armed = false;
    bool paused = false;
    bool released = false;
};

/// Message IDs Receive handed out, in order.
struct Received
{
    std::vector<MessageID> ids;

    void Drain( RakPeerInterface& peer )
    {
        for( Packet* packet = peer.Receive(); packet != 0; packet = peer.Receive() )
        {
            if( packet->length > 0 )
                ids.push_back( packet->data[0] );
            peer.DeallocatePacket( packet );
        }
    }

    int Count( MessageID id ) const
    {
        int count = 0;
        for( MessageID each : ids )
        {
            if( each == id )
                count++;
        }
        return count;
    }
};

} // namespace

TEST_CASE( "Connect to a System the view shows connected returns ALREADY_CONNECTED_TO_ENDPOINT", "[network]" )
{
    PausingPeer b;
    b.Start( kConnectedServerPort );
    PausingPeer a;
    a.Start( kConnectedClientPort );
    Received aReceived;
    Received bReceived;

    REQUIRE( a.Connect( "127.0.0.1", kConnectedServerPort, 0, 0 ) == CONNECTION_ATTEMPT_STARTED );
    REQUIRE( ConnectionWaits::WaitUntil(
        [&] {
            aReceived.Drain( a );
            bReceived.Drain( b );
            return a.GetConnectionState( b.Address() ) == IS_CONNECTED;
        },
        kWaitBudgetMs ) );

    CHECK( a.Connect( "127.0.0.1", kConnectedServerPort, 0, 0 ) == ALREADY_CONNECTED_TO_ENDPOINT );

    std::vector<RakNetSocket2*> sockets;
    a.GetSockets( sockets );
    REQUIRE( sockets.size() == 1 );
    CHECK( a.ConnectWithSocket( "127.0.0.1", kConnectedServerPort, 0, 0, sockets[0] ) == ALREADY_CONNECTED_TO_ENDPOINT );

    CHECK_FALSE( a.AttemptQueued( b.Address() ) );
}

TEST_CASE( "Connect accepted while the view lags leaves one connection and one ID_CONNECTION_REQUEST_ACCEPTED", "[network]" )
{
    PausingPeer b;
    b.Start( kStaleServerPort );
    PausingPeer a;
    a.Start( kStaleClientPort );
    Received aReceived;
    Received bReceived;

    a.PauseWhenUnpublished( b.Address() );
    REQUIRE( a.Connect( "127.0.0.1", kStaleServerPort, 0, 0 ) == CONNECTION_ATTEMPT_STARTED );
    REQUIRE( a.WaitUntilPaused() );

    // A's network thread holds an open record for B that the view does not show yet.
    const ConnectionAttemptResult second = a.Connect( "127.0.0.1", kStaleServerPort, 0, 0, 0, 0, kAttemptCount, kAttemptIntervalMs );
    a.Release();
    REQUIRE( second == CONNECTION_ATTEMPT_STARTED );

    REQUIRE( ConnectionWaits::WaitUntil(
        [&] {
            aReceived.Drain( a );
            bReceived.Drain( b );
            return a.GetConnectionState( b.Address() ) == IS_CONNECTED && b.GetConnectionState( a.Address() ) == IS_CONNECTED;
        },
        kWaitBudgetMs ) );
    // The second attempt leaves the queue once the network thread has acted on it or
    // given up on it.
    REQUIRE( ConnectionWaits::WaitUntil(
        [&] {
            aReceived.Drain( a );
            bReceived.Drain( b );
            return a.AttemptQueued( b.Address() ) == false;
        },
        kWaitBudgetMs ) );
    ConnectionWaits::WaitUntil(
        [&] {
            aReceived.Drain( a );
            bReceived.Drain( b );
            return false;
        },
        kSecondAttemptSettleMs );

    CHECK( a.NumberOfConnections() == 1 );
    CHECK( b.NumberOfConnections() == 1 );
    CHECK( a.GetConnectionState( b.Address() ) == IS_CONNECTED );
    CHECK( aReceived.Count( ID_CONNECTION_REQUEST_ACCEPTED ) == 1 );
    CHECK( bReceived.Count( ID_NEW_INCOMING_CONNECTION ) == 1 );
    CHECK( aReceived.Count( ID_CONNECTION_LOST ) == 0 );
    CHECK( aReceived.Count( ID_DISCONNECTION_NOTIFICATION ) == 0 );
}
