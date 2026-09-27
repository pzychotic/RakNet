#include "Plugins/NatTypeDetectionClient.h"
#include "Plugins/NatTypeDetectionServer.h"

#include "PeerScope.h"
#include "RakNetSocket2.h"
#include "RakNetTypes.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

/*
NatTypeDetectionServer and NatTypeDetectionClient buffer every datagram their own sockets
receive, from a recv polling thread, and drain that buffer in Update, which RakPeer::Receive
runs. The drain loop pops one struct, handles it, frees it, and pops the next. When the
buffer ran dry it used to leave the pointer at the struct it had just freed, so the loop
never ended: it read the freed struct, freed it again, and spun inside Receive forever. One
datagram from anyone was enough.

Each plugin is wrapped in a subclass that keeps the set of structs it has handed out and not
yet had back. A second free of the same struct is the bug, and the loop would never return
to report it, so the subclass aborts on the spot instead of hanging until ctest's timeout.

The datagrams are sent from the peer's own socket straight to the plugin's, and are not
RakNet messages. The payload byte is none of the NATTypeDetectionResult values either plugin
acts on, so the drain loop only has to pop and free.

The peer is bound to 127.0.0.1, not the wildcard address PeerScope uses. Both plugins bind a
socket to the peer's own address, and a polling thread is stopped by sending its socket a
datagram, which cannot be sent to 0.0.0.0. Bound there, the threads outlive their sockets
and the plugin, and the test crashes in teardown.
*/

using namespace RakNet;

namespace
{

const unsigned char kPayload = 0xFF;
const std::chrono::seconds kBufferDeadline( 5 );

/// The alloc/free bookkeeping both subclasses share. Called from the plugin's polling
/// threads and from Update, so everything is under one mutex.
class RecvStructLedger
{
public:
    void Allocated( RNS2RecvStruct* s )
    {
        std::lock_guard<std::mutex> guard( m_mutex );
        m_live.insert( s );
    }

    // Aborts on a struct that is not live.
    void Freed( RNS2RecvStruct* s )
    {
        std::lock_guard<std::mutex> guard( m_mutex );
        if( m_live.erase( s ) == 0 )
        {
            std::fprintf( stderr, "RNS2RecvStruct %p freed twice: the drain loop reused a freed struct\n", (void*)s );
            std::abort();
        }
        m_bufferedLive.erase( s );
    }

    // Only the test's own datagrams: Bind sends every socket a test datagram of its own,
    // which the drain loop pops and frees like the rest.
    static bool IsTestDatagram( const RNS2RecvStruct* s )
    {
        return s->bytesRead == 1 && (unsigned char)s->data[0] == kPayload;
    }

    // Before the plugin buffers s: once it has, Update may free it at any moment.
    void Buffering( RNS2RecvStruct* s )
    {
        std::lock_guard<std::mutex> guard( m_mutex );
        m_bufferedLive.insert( s );
    }

    // After the plugin has buffered it, so a count that has reached n means Update will
    // find all n.
    void Buffered()
    {
        std::lock_guard<std::mutex> guard( m_mutex );
        ++m_bufferedCount;
    }

    size_t BufferedCount()
    {
        std::lock_guard<std::mutex> guard( m_mutex );
        return m_bufferedCount;
    }

    // How many of the buffered structs have not been freed yet.
    size_t BufferedStillLive()
    {
        std::lock_guard<std::mutex> guard( m_mutex );
        return m_bufferedLive.size();
    }

private:
    std::mutex m_mutex;
    std::set<RNS2RecvStruct*> m_live;

    // Its own set rather than a lookup of buffered addresses in m_live: a polling thread's
    // next allocation can reuse the address of a struct Update has just freed.
    std::set<RNS2RecvStruct*> m_bufferedLive;
    size_t m_bufferedCount = 0;
};

/// Either plugin, with every struct it allocates, frees and buffers reported to a ledger.
template<class Plugin>
class Ledgered : public Plugin
{
public:
    RNS2RecvStruct* AllocRNS2RecvStruct( const char* file, unsigned int line ) override
    {
        RNS2RecvStruct* s = Plugin::AllocRNS2RecvStruct( file, line );
        ledger.Allocated( s );
        return s;
    }

    void DeallocRNS2RecvStruct( RNS2RecvStruct* s, const char* file, unsigned int line ) override
    {
        ledger.Freed( s );
        Plugin::DeallocRNS2RecvStruct( s, file, line );
    }

    void OnRNS2Recv( RNS2RecvStruct* s ) override
    {
        const bool isTest = RecvStructLedger::IsTestDatagram( s );
        if( isTest )
            ledger.Buffering( s );
        Plugin::OnRNS2Recv( s );
        if( isTest )
            ledger.Buffered();
    }

    RecvStructLedger ledger;
};

// Declared before the PeerScope in each test, so the plugin outlives the peer it is attached to.
class LedgerServer : public Ledgered<NatTypeDetectionServer>
{
public:
    ~LedgerServer() override
    {
        // NatTypeDetectionServer::Shutdown stops only s3p4's polling thread before deleting
        // its sockets, and the other three threads go on using their deleted socket and this
        // plugin. Stop them here, and then the rest while this subclass and its ledger, which
        // the threads call into, still exist.
        for( RakNetSocket2* s : { s1p2, s2p3, s4p5 } )
        {
            if( s != nullptr && s->IsBerkleySocket() )
                static_cast<RNS2_Berkley*>( s )->BlockOnStopRecvPollingThread();
        }
        Shutdown();
    }

    unsigned short S3P4Port() const { return s3p4->GetBoundAddress().GetPort(); }
};

class LedgerClient : public Ledgered<NatTypeDetectionClient>
{
public:
    ~LedgerClient() override
    {
        // Normally a no-op: detaching the plugin, or the peer shutting down, has already run
        // it. Here for a test that fails before either, while the ledger still exists.
        Shutdown();
    }

    bool InProgress() const { return IsInProgress(); }
    unsigned short C2Port() const { return c2->GetBoundAddress().GetPort(); }
};

RakPeerInterface* StartLoopbackPeer( PeerScope& peers )
{
    RakPeerInterface* peer = peers.Create();
    SocketDescriptor socketDescriptor( 0, "127.0.0.1" );
    REQUIRE( peer->Startup( 1, &socketDescriptor, 1 ) == RAKNET_STARTED );
    return peer;
}

void SendRawDatagrams( RakPeerInterface* peer, unsigned short port, int count )
{
    std::vector<RakNetSocket2*> sockets;
    peer->GetSockets( sockets );
    REQUIRE( !sockets.empty() );

    char data = (char)kPayload;
    RNS2_SendParameters bsp;
    bsp.data = &data;
    bsp.length = 1;
    bsp.systemAddress = SystemAddress( "127.0.0.1", port );
    for( int i = 0; i < count; ++i )
        REQUIRE( sockets.front()->Send( &bsp, _FILE_AND_LINE_ ) > 0 );
}

// Until the plugin's polling thread has buffered count datagrams, so the Receive that
// follows has them all to drain.
void WaitUntilBuffered( RecvStructLedger& ledger, size_t count )
{
    const auto deadline = std::chrono::steady_clock::now() + kBufferDeadline;
    while( ledger.BufferedCount() < count && std::chrono::steady_clock::now() < deadline )
        std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
    REQUIRE( ledger.BufferedCount() == count );
}

void ReceiveOnce( RakPeerInterface* peer )
{
    // Runs every plugin's Update. Before the fix it never came back.
    Packet* packet = peer->Receive();
    if( packet != nullptr )
        peer->DeallocatePacket( packet );
}

} // namespace

TEST_CASE( "NatTypeDetectionServer Update drains its datagram buffer and returns", "[nattypedetection][network]" )
{
    const int count = GENERATE( 1, 3 );

    LedgerServer server;

    PeerScope peers;
    RakPeerInterface* peer = StartLoopbackPeer( peers );
    peer->AttachPlugin( &server );
    server.Startup( "127.0.0.1", "127.0.0.1", "127.0.0.1" );

    SendRawDatagrams( peer, server.S3P4Port(), count );
    WaitUntilBuffered( server.ledger, (size_t)count );

    ReceiveOnce( peer );

    CHECK( server.ledger.BufferedStillLive() == 0 );

    peer->DetachPlugin( &server );
}

TEST_CASE( "NatTypeDetectionClient Update drains its datagram buffer and returns", "[nattypedetection][network]" )
{
    const int count = GENERATE( 1, 3 );

    LedgerClient client;

    PeerScope peers;
    RakPeerInterface* peer = StartLoopbackPeer( peers );
    peer->AttachPlugin( &client );

    // Nothing listens there. Update only drains while a detection is in progress, and
    // DetectNATType is what starts one and binds the socket the datagrams go to.
    client.DetectNATType( SystemAddress( "127.0.0.1", 1 ) );
    REQUIRE( client.InProgress() );

    SendRawDatagrams( peer, client.C2Port(), count );
    WaitUntilBuffered( client.ledger, (size_t)count );

    ReceiveOnce( peer );

    CHECK( client.ledger.BufferedStillLive() == 0 );
    CHECK( client.InProgress() );

    peer->DetachPlugin( &client );
}
