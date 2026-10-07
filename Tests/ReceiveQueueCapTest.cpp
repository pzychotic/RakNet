#include "PeerScope.h"
#include "RawSystem.h"

#include "ConnectionWaits.h"
#include "GetTime.h"
#include "MessageIdentifiers.h"
#include "RakNetDefines.h"
#include "RakPeer.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

/*
Pins the caps on RakPeer's two receive queues that dropping cannot hurt (ADR-0005, point 7).

Before them, a sender that never connected could grow both. The receive thread queued every
datagram for the update thread with no limit, so a flood outran an update thread that was
busy or stalled. And every unconnected ping, pong and out-of-band datagram became a Packet
waiting for Receive, with no limit on how many.

The shapes pinned here:

- With the update thread held, a flood leaves MAX_BUFFERED_RECEIVED_DATAGRAMS datagrams
  queued and drops the rest, and once the update thread runs again every datagram's
  buffer is back in the pool.
- With Receive not called, a flood of unconnected pings stops at
  MAX_PENDING_OFFLINE_MESSAGES, while every Message a connected System sends meanwhile
  still arrives.
- A datagram the incoming-datagram handler rejects has its buffer returned. It used to
  leak, one buffer per rejected datagram.

RakPeerInterface functions explicitly tested:

    GetReceivedDatagramsDroppedAtCap
    GetOfflineMessagesDroppedAtCap
    SetIncomingDatagramEventHandler
    SetUserUpdateThread
    GetReceiveBufferSize
*/

using namespace RakNet;
using RawSystemHarness::RawSystem;

namespace
{

// Hang guard for every wait below, not a settle time: each normally ends within a few
// hundred milliseconds.
constexpr TimeMS kWaitBudgetMs = 10000;

/// A RakPeer that counts the receive buffers its receive thread takes and gives back, and
/// shows how many datagrams wait for the update thread. Both are protected in RakPeer.
///
/// Built directly rather than through PeerScope, which hands out RakPeerInterface only.
class ObservedPeer : public RakPeer
{
public:
    ~ObservedPeer() override
    {
        // The receive thread calls the overrides below, so it has to be gone before this
        // part of the object is. ~RakPeer's own Shutdown is then a no-op.
        Shutdown( 0 );
    }

    void Start()
    {
        SocketDescriptor socketDescriptor( 0, "127.0.0.1" );
        REQUIRE( Startup( 1, &socketDescriptor, 1 ) == RAKNET_STARTED );
    }

    SystemAddress Address()
    {
        SystemAddress address = GetMyBoundAddress();
        address.FromStringExplicitPort( "127.0.0.1", address.GetPort() );
        return address;
    }

    size_t BufferedDatagrams()
    {
        std::lock_guard<std::mutex> guard( bufferedPacketsQueueMutex );
        return bufferedPacketsQueue.size();
    }

    /// Receive buffers handed out and not given back. The receive thread always holds one
    /// while it waits in recvfrom, so an idle Peer on one socket reads 1.
    long long BuffersOutstanding() const
    {
        return allocated.load() - deallocated.load();
    }

    RNS2RecvStruct* AllocRNS2RecvStruct( const char* file, unsigned int line ) override
    {
        ++allocated;
        return RakPeer::AllocRNS2RecvStruct( file, line );
    }

    void DeallocRNS2RecvStruct( RNS2RecvStruct* s, const char* file, unsigned int line ) override
    {
        ++deallocated;
        RakPeer::DeallocRNS2RecvStruct( s, file, line );
    }

private:
    std::atomic<long long> allocated{ 0 };
    std::atomic<long long> deallocated{ 0 };
};

/// Holds a Peer's update thread inside its user-update callback until released. Declared
/// after the Peer it holds, so it is released before the Peer shuts down.
class UpdateThreadGate
{
public:
    explicit UpdateThreadGate( RakPeerInterface* peer )
    : m_peer( peer )
    {
        {
            std::lock_guard<std::mutex> guard( m_mutex );
            m_held = true;
        }
        peer->SetUserUpdateThread( &UpdateThreadGate::Callback, this );

        std::unique_lock<std::mutex> lock( m_mutex );
        REQUIRE( m_changed.wait_for( lock, std::chrono::milliseconds( kWaitBudgetMs ), [this] { return m_waiting; } ) );
    }

    ~UpdateThreadGate()
    {
        Release();
        m_peer->SetUserUpdateThread( nullptr, nullptr );
    }

    void Release()
    {
        std::lock_guard<std::mutex> guard( m_mutex );
        m_held = false;
        m_changed.notify_all();
    }

private:
    static void Callback( RakPeerInterface*, void* data )
    {
        UpdateThreadGate* gate = static_cast<UpdateThreadGate*>( data );
        std::unique_lock<std::mutex> lock( gate->m_mutex );
        gate->m_waiting = true;
        gate->m_changed.notify_all();
        gate->m_changed.wait( lock, [gate] { return gate->m_held == false; } );
    }

    RakPeerInterface* m_peer;
    std::mutex m_mutex;
    std::condition_variable m_changed;
    bool m_held = false;
    bool m_waiting = false;
};

constexpr int kJunkDatagramBytes = 2;

/// Two bytes that are no RakNet message: ProcessNetworkPacket treats them as offline and
/// discards them, so a flood of them costs the update thread nothing once it runs.
void WriteJunkDatagram( BitStream& datagram )
{
    datagram.Write( (MessageID)ID_USER_PACKET_ENUM );
    datagram.Write( (MessageID)0 );
    REQUIRE( datagram.GetNumberOfBytesUsed() == kJunkDatagramBytes );
}

/// An ID_UNCONNECTED_PING, field for field as RakPeer::Ping writes it.
void WriteUnconnectedPing( BitStream& datagram )
{
    datagram.Write( (MessageID)ID_UNCONNECTED_PING );
    datagram.Write( RakNet::GetTime() );
    datagram.WriteAlignedBytes( RawSystemHarness::OFFLINE_MESSAGE_DATA_ID, sizeof( RawSystemHarness::OFFLINE_MESSAGE_DATA_ID ) );
    datagram.Write( RakNetGUID( 0x5eed ) );
}

std::atomic<int> s_rejectedJunkDatagrams{ 0 };

/// Counts only the junk datagrams the test sends. Startup's own bind-test datagram can still be
/// waiting in the socket when the handler is installed, and is rejected uncounted.
bool RejectEveryDatagram( RNS2RecvStruct* recvStruct )
{
    if( recvStruct->bytesRead == kJunkDatagramBytes )
        ++s_rejectedJunkDatagrams;
    return false;
}

} // namespace

TEST_CASE( "A datagram flood with the update thread held stops at MAX_BUFFERED_RECEIVED_DATAGRAMS", "[network]" )
{
    ObservedPeer peer;
    peer.Start();
    RawSystem sender( peer.Address(), 0x51 );

    {
        UpdateThreadGate gate( &peer );

        // Batches, not one burst, so the receive thread keeps up and it is the cap that
        // drops rather than the socket buffer. Until the cap has dropped something.
        BitStream junk;
        WriteJunkDatagram( junk );
        for( int batch = 0; batch < 64 && peer.GetReceivedDatagramsDroppedAtCap() == 0; ++batch )
        {
            for( int i = 0; i < MAX_BUFFERED_RECEIVED_DATAGRAMS / 16; ++i )
                sender.Send( junk );
            std::this_thread::sleep_for( std::chrono::milliseconds( 2 ) );
        }
        REQUIRE( ConnectionWaits::WaitUntil( [&] { return peer.GetReceivedDatagramsDroppedAtCap() > 0; }, kWaitBudgetMs ) );

        CHECK( peer.BufferedDatagrams() == (size_t)MAX_BUFFERED_RECEIVED_DATAGRAMS );

        // More arrivals are dropped as they come, and the queue stays where it is.
        const uint64_t droppedBefore = peer.GetReceivedDatagramsDroppedAtCap();
        for( int i = 0; i < 16; ++i )
            sender.Send( junk );
        REQUIRE( ConnectionWaits::WaitUntil( [&] { return peer.GetReceivedDatagramsDroppedAtCap() >= droppedBefore + 16; }, kWaitBudgetMs ) );
        CHECK( peer.BufferedDatagrams() == (size_t)MAX_BUFFERED_RECEIVED_DATAGRAMS );

        gate.Release();
    }

    REQUIRE( ConnectionWaits::WaitUntil( [&] { return peer.BufferedDatagrams() == 0; }, kWaitBudgetMs ) );
    CHECK( ConnectionWaits::WaitUntil( [&] { return peer.BuffersOutstanding() == 1; }, kWaitBudgetMs ) );
}

TEST_CASE( "A flood of unconnected pings stops at MAX_PENDING_OFFLINE_MESSAGES and a connected System's Messages all arrive", "[network]" )
{
    PeerScope peers;
    RakPeerInterface* server = peers.Server( 30000, 1 );
    RakPeerInterface* client = peers.Client();

    REQUIRE( client->Connect( "127.0.0.1", 30000, 0, 0 ) == CONNECTION_ATTEMPT_STARTED );
    RakPeerInterface* both[] = { server, client };
    ConnectionWaits::WaitForConnectionCounts( both, 2, 1 );
    ConnectionWaits::Drain( server );
    REQUIRE( ConnectionWaits::WaitUntil( [&] { return server->GetReceiveBufferSize() == 0; }, kWaitBudgetMs ) );

    RawSystem pinger( SystemAddress( "127.0.0.1", 30000 ), 0x52 );
    BitStream ping;
    WriteUnconnectedPing( ping );

    // Receive is not called on the server from here until the counts are read. The
    // client's Messages go out between the batches, so they arrive during the flood.
    const int kConnectedMessages = 200;
    int connectedSent = 0;
    for( int batch = 0; batch < 64 && ( server->GetOfflineMessagesDroppedAtCap() == 0 || connectedSent < kConnectedMessages ); ++batch )
    {
        for( int i = 0; i < MAX_PENDING_OFFLINE_MESSAGES / 8; ++i )
            pinger.Send( ping );

        for( int i = 0; i < kConnectedMessages / 20 && connectedSent < kConnectedMessages; ++i, ++connectedSent )
        {
            BitStream message;
            message.Write( (MessageID)ID_USER_PACKET_ENUM );
            message.Write( connectedSent );
            REQUIRE( client->Send( &message, HIGH_PRIORITY, RELIABLE_ORDERED, 0, server->GetMyGUID(), false ) != 0 );
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( 2 ) );
    }
    REQUIRE( connectedSent == kConnectedMessages );
    REQUIRE( ConnectionWaits::WaitUntil( [&] { return server->GetOfflineMessagesDroppedAtCap() > 0; }, kWaitBudgetMs ) );
    REQUIRE( ConnectionWaits::WaitUntil(
        [&] { return server->GetReceiveBufferSize() >= (unsigned int)( MAX_PENDING_OFFLINE_MESSAGES + kConnectedMessages ); }, kWaitBudgetMs ) );

    int pings = 0;
    int connectedReceived = 0;
    for( Packet* packet = server->Receive(); packet != 0; packet = server->Receive() )
    {
        if( packet->data[0] == ID_UNCONNECTED_PING )
        {
            ++pings;
        }
        else if( packet->data[0] == ID_USER_PACKET_ENUM )
        {
            BitStream in( packet->data, packet->length, false );
            in.IgnoreBytes( sizeof( MessageID ) );
            int index = -1;
            REQUIRE( in.Read( index ) );
            CHECK( index == connectedReceived );
            ++connectedReceived;
        }
        server->DeallocatePacket( packet );
    }

    CHECK( pings == MAX_PENDING_OFFLINE_MESSAGES );
    CHECK( connectedReceived == kConnectedMessages );

    // Draining frees the slots: the next ping is queued again.
    const uint64_t droppedBefore = server->GetOfflineMessagesDroppedAtCap();
    pinger.Send( ping );
    REQUIRE( ConnectionWaits::WaitUntil( [&] { return server->GetReceiveBufferSize() == 1; }, kWaitBudgetMs ) );
    CHECK( server->GetOfflineMessagesDroppedAtCap() == droppedBefore );
}

TEST_CASE( "A datagram the incoming-datagram handler rejects gives its buffer back", "[network]" )
{
    ObservedPeer peer;
    peer.Start();
    s_rejectedJunkDatagrams = 0;
    peer.SetIncomingDatagramEventHandler( &RejectEveryDatagram );

    RawSystem sender( peer.Address(), 0x53 );
    BitStream junk;
    WriteJunkDatagram( junk );
    const int kDatagrams = 100;
    for( int i = 0; i < kDatagrams; ++i )
        sender.Send( junk );

    REQUIRE( ConnectionWaits::WaitUntil( [&] { return s_rejectedJunkDatagrams.load() == kDatagrams; }, kWaitBudgetMs ) );
    CHECK( ConnectionWaits::WaitUntil( [&] { return peer.BuffersOutstanding() == 1; }, kWaitBudgetMs ) );
    CHECK( peer.BufferedDatagrams() == 0 );

    peer.SetIncomingDatagramEventHandler( nullptr );
}

TEST_CASE( "Shutdown gives every receive buffer back through DeallocRNS2RecvStruct", "[network]" )
{
    ObservedPeer peer;
    peer.Start();
    REQUIRE( ConnectionWaits::WaitUntil( [&] { return peer.BuffersOutstanding() == 1; }, kWaitBudgetMs ) );

    // Shutdown wakes the receive thread with a datagram to the socket's own address, and
    // the receive thread queues it after the update thread is gone. That buffer is freed
    // by Shutdown, and has to come back through the override like every other one.
    peer.Shutdown( 0 );
    CHECK( peer.BuffersOutstanding() == 0 );
}
