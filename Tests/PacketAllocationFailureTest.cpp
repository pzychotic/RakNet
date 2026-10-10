#include "PeerScope.h"

#include "ConnectionWaits.h"
#include "MessageIdentifiers.h"
#include "RakAssert.h"
#include "RakMemoryOverride.h"
#include "RakNetTypes.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstring>
#include <vector>

/*
RakPeer builds every Packet it hands the user in two steps: the payload from rakMalloc_Ex,
then the Packet itself with OP_NEW. The payload can come back null. When it does, the Packet
is not built: notifyOutOfMemory is told, the message that needed it is dropped, and the
Peer carries on. ADR-0004, "Null-returning allocators". Failing to make the Packet itself
is fatal, like any other allocation.

RakPeerInterface functions explicitly tested:

    AllocatePacket
    SendLoopback
    Receive
    DeallocatePacket
*/

using namespace RakNet;

namespace
{

/// Fails every rakMalloc_Ex of exactly one byte count while in scope, and counts the
/// notifyOutOfMemory calls made meanwhile.
///
/// Global and not thread-safe, so it is installed before any Peer it watches starts and
/// removed after that Peer is gone: declare it before the PeerScope. The byte counts the
/// cases below fail are chosen so that nothing else in the process asks for them.
class FailingMalloc
{
public:
    explicit FailingMalloc( size_t failSize )
    {
        RakAssert( s_previousMalloc == nullptr );
        s_failSize = failSize;
        s_failures = 0;
        s_outOfMemoryNotices = 0;
        s_previousMalloc = GetMalloc_Ex();
        s_previousNotify = notifyOutOfMemory;
        SetMalloc_Ex( &Intercept );
        SetNotifyOutOfMemory( &CountOutOfMemory );
    }

    ~FailingMalloc()
    {
        SetMalloc_Ex( s_previousMalloc );
        SetNotifyOutOfMemory( s_previousNotify );
        s_previousMalloc = nullptr;
        s_failSize = 0;
    }

    FailingMalloc( const FailingMalloc& ) = delete;
    FailingMalloc& operator=( const FailingMalloc& ) = delete;

    /// Stop failing, so the case can show the Peer recovers. The notice count is kept.
    static void Stop() { s_failSize = 0; }

    static int Failures() { return s_failures; }
    static int OutOfMemoryNotices() { return s_outOfMemoryNotices; }

private:
    static void* Intercept( size_t size, const char* file, unsigned int line )
    {
        if( s_failSize != 0 && size == s_failSize )
        {
            ++s_failures;
            return nullptr;
        }
        return s_previousMalloc( size, file, line );
    }

    static void CountOutOfMemory( const char*, const long ) { ++s_outOfMemoryNotices; }

    static inline void* ( *s_previousMalloc )( size_t, const char*, unsigned int ) = nullptr;
    static inline void ( *s_previousNotify )( const char*, const long ) = nullptr;
    static inline size_t s_failSize = 0;
    static inline int s_failures = 0;
    static inline int s_outOfMemoryNotices = 0;
};

/// A loopback payload of \a size bytes: the message id, then a fill byte.
std::vector<char> LoopbackPayload( size_t size, char fill )
{
    std::vector<char> payload( size, fill );
    payload[0] = (char)( ID_USER_PACKET_ENUM + 1 );
    return payload;
}

/// Receive for up to \a millisecondsToWait; true if a packet of exactly \a payload
/// arrived. Every packet ahead of it is discarded.
bool ReceivesPayload( RakPeerInterface* peer, const std::vector<char>& payload, TimeMS millisecondsToWait )
{
    return ConnectionWaits::WaitForMessage( peer, (MessageID)payload[0], millisecondsToWait, [&]( const Packet& packet ) {
        return packet.length == payload.size() && memcmp( packet.data, payload.data(), payload.size() ) == 0;
    } );
}

// Odd sizes no other allocation in a Peer asks for.
constexpr size_t kFailedPayloadBytes = 1237;
constexpr size_t kLaterPayloadBytes = 1239;

} // namespace

TEST_CASE( "AllocatePacket returns null when the payload allocation fails, and the next one succeeds", "[memory]" )
{
    FailingMalloc failing( kFailedPayloadBytes );
    PeerScope peers;
    RakPeerInterface* peer = peers.Create();

    CHECK( peer->AllocatePacket( (unsigned)kFailedPayloadBytes ) == nullptr );
    CHECK( FailingMalloc::Failures() == 1 );
    CHECK( FailingMalloc::OutOfMemoryNotices() == 1 );

    FailingMalloc::Stop();
    Packet* packet = peer->AllocatePacket( (unsigned)kFailedPayloadBytes );
    REQUIRE( packet != nullptr );
    CHECK( packet->length == kFailedPayloadBytes );
    peer->DeallocatePacket( packet );
}

TEST_CASE( "A SendLoopback whose packet cannot be allocated is dropped, and the next one arrives", "[memory][network]" )
{
    FailingMalloc failing( kFailedPayloadBytes );
    PeerScope peers;
    RakPeerInterface* peer = peers.Client();

    const std::vector<char> dropped = LoopbackPayload( kFailedPayloadBytes, 'd' );
    peer->SendLoopback( dropped.data(), (int)dropped.size() );
    CHECK( FailingMalloc::Failures() == 1 );
    CHECK( FailingMalloc::OutOfMemoryNotices() == 1 );
    CHECK_FALSE( ReceivesPayload( peer, dropped, 100 ) );

    const std::vector<char> later = LoopbackPayload( kLaterPayloadBytes, 'l' );
    peer->SendLoopback( later.data(), (int)later.size() );
    CHECK( ReceivesPayload( peer, later, 1000 ) );
}
