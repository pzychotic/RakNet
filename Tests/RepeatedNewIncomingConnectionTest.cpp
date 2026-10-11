#include "PeerScope.h"
#include "RawSystem.h"

#include "BitStream.h"
#include "ConnectionWaits.h"
#include "GetTime.h"
#include "MessageIdentifiers.h"
#include "RakAssert.h"
#include "RakMemoryOverride.h"
#include "RakNetTypes.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <mutex>
#include <unordered_set>

/*
A System that is already connected can send ID_NEW_INCOMING_CONNECTION again. The far
side acts on it only while the connection is still being opened. On an open connection
the message is freed and dropped: the application is never told one connection opened
twice, and a System cannot hold memory or fill Receive's queue by repeating it.

The payload is counted through SetMalloc_Ex and SetFree_Ex rather than left to a leak
checker, so the case goes red in any build.

RakPeerInterface functions explicitly tested:

    Send (of a hand-built ID_NEW_INCOMING_CONNECTION)
    Receive

Exercised indirectly by getting to that point: Startup, SetMaximumIncomingConnections,
Connect, GetGuidFromSystemAddress, DeallocatePacket.
*/

using namespace RakNet;

namespace {

constexpr unsigned short kServerPort = 30000;

constexpr int kRepeats = 8;

// Every message travels one hop over loopback, and its ack one hop back. Generous by two
// orders of magnitude, and a hang guard rather than a tuning knob.
constexpr TimeMS kBudgetMs = 5000;

/// Tracks every rakMalloc_Ex of exactly one byte count made after Arm, and forgets each
/// one rakFree_Ex returns. Outstanding is how many are still held.
///
/// Global, so it is installed before any Peer it watches starts and removed after that
/// Peer is gone: declare it before the PeerScope. The hooks run on the network thread;
/// the set is guarded.
class OutstandingAllocations
{
public:
    OutstandingAllocations()
    {
        RakAssert( s_previousMalloc == nullptr );
        s_previousMalloc = GetMalloc_Ex();
        s_previousFree = GetFree_Ex();
        SetMalloc_Ex( &Malloc );
        SetFree_Ex( &Free );
    }

    ~OutstandingAllocations()
    {
        SetMalloc_Ex( s_previousMalloc );
        SetFree_Ex( s_previousFree );
        s_previousMalloc = nullptr;
        s_previousFree = nullptr;
        std::lock_guard<std::mutex> lock( s_mutex );
        s_trackedSize = 0;
        s_outstanding.clear();
    }

    OutstandingAllocations( const OutstandingAllocations& ) = delete;
    OutstandingAllocations& operator=( const OutstandingAllocations& ) = delete;

    /// Start tracking allocations of \a size bytes.
    static void Arm( size_t size )
    {
        std::lock_guard<std::mutex> lock( s_mutex );
        s_trackedSize = size;
    }

    static size_t Outstanding()
    {
        std::lock_guard<std::mutex> lock( s_mutex );
        return s_outstanding.size();
    }

private:
    static void* Malloc( size_t size, const char* file, unsigned int line )
    {
        void* p = s_previousMalloc( size, file, line );
        std::lock_guard<std::mutex> lock( s_mutex );
        if( p != nullptr && s_trackedSize != 0 && size == s_trackedSize )
            s_outstanding.insert( p );
        return p;
    }

    static void Free( void* p, const char* file, unsigned int line )
    {
        {
            std::lock_guard<std::mutex> lock( s_mutex );
            s_outstanding.erase( p );
        }
        s_previousFree( p, file, line );
    }

    static inline void* ( *s_previousMalloc )( size_t, const char*, unsigned int ) = nullptr;
    static inline void ( *s_previousFree )( void*, const char*, unsigned int ) = nullptr;
    static inline std::mutex s_mutex;
    static inline size_t s_trackedSize = 0;
    static inline std::unordered_set<void*> s_outstanding;
};

} // namespace

TEST_CASE( "A repeated ID_NEW_INCOMING_CONNECTION on an open connection is freed and not delivered", "[network][memory]" )
{
    OutstandingAllocations tracking;
    PeerScope peers;

    RakPeerInterface* server = peers.Server( kServerPort );
    RakPeerInterface* client = peers.Client();

    // Drains both ends, so the server has had the handshake's own
    // ID_NEW_INCOMING_CONNECTION and the connection is open on both.
    ConnectionWaits::ConnectAndWait( client, server );

    const SystemAddress serverAddress( "127.0.0.1", kServerPort );
    const RakNetGUID serverGuid = client->GetGuidFromSystemAddress( serverAddress );
    REQUIRE( serverGuid != UNASSIGNED_RAKNET_GUID );

    BitStream repeat;
    RawSystemHarness::WriteNewIncomingConnection( repeat, serverAddress, ConnectionWaits::LoopbackAddressOf( client ), RakNet::GetTime(), RakNet::GetTime() );

    // The sender's resend copy and the receiver's payload are both this size. The first
    // is freed when the ack arrives, the second only if the receiver frees it.
    OutstandingAllocations::Arm( repeat.GetNumberOfBytesUsed() );

    for( int i = 0; i < kRepeats; i++ )
        REQUIRE( client->Send( &repeat, IMMEDIATE_PRIORITY, RELIABLE_ORDERED, 0, serverGuid, false ) != 0 );

    // Behind the repeats on the same ordering channel, so the server has handled every
    // repeat by the time it receives this.
    const unsigned char marker = ID_USER_PACKET_ENUM;
    REQUIRE( client->Send( (const char*)&marker, 1, IMMEDIATE_PRIORITY, RELIABLE_ORDERED, 0, serverGuid, false ) != 0 );

    int delivered = 0;
    bool markerArrived = false;
    ConnectionWaits::WaitUntil(
        [&] {
            for( Packet* packet = server->Receive(); packet != nullptr; packet = server->Receive() )
            {
                if( packet->data[0] == ID_NEW_INCOMING_CONNECTION )
                    ++delivered;
                else if( packet->data[0] == ID_USER_PACKET_ENUM )
                    markerArrived = true;
                server->DeallocatePacket( packet );
            }
            return markerArrived;
        },
        kBudgetMs );
    REQUIRE( markerArrived );

    CHECK( delivered == 0 );

    RakPeerInterface* const both[] = { client, server };
    const bool released = ConnectionWaits::DrainUntil( both, 2, [] { return OutstandingAllocations::Outstanding() == 0; }, kBudgetMs );
    INFO( "payloads still held: " << OutstandingAllocations::Outstanding() << " of " << kRepeats );
    CHECK( released );
}
