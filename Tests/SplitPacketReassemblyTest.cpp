#include "PeerScope.h"

#include "GetTime.h"
#include "RakMemoryOverride.h"
#include "RakNetSocket2.h"
#include "Rand.h"
#include "ReliabilityLayer.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

/*
Drives ReliabilityLayer's split-packet reassembly path with hand-built datagrams,
the way a hostile System would rather than the way RakNet's own sender does.

splitPacketCount arrives as an unvalidated uint32_t. The pointer array
SortedSplittedPackets::Preallocate sizes from it is filled with NULL, so every page
is touched, and the wire minimum for a split message is 14 bytes - so one datagram
buys an allocation of up to 16 GiB per channel and 105 channels fit in an MTU. The
first four cases below pin the bound (MAXIMUM_SPLIT_PACKET_COUNT), the rejection of
a count above it, the rejection of a count that would arrive negative through
OP_NEW_ARRAY's int parameter, and what happens when the capped allocation still fails.

The last case pins the other half: a channel whose remaining chunks never arrive is
reaped on a timer rather than held until the connection resets, for every reliability
level - a sweep conditional on reliability is evaded by marking the chunks RELIABLE.

These tests build datagrams by hand rather than going through a Peer because there is
no supported way to ask a Peer to send a malformed one. ReliabilityLayer is driven
directly: HandleSocketReceiveFromConnectedPlayer, Update and Receive are all public,
and Update takes the current time as a parameter, so the reaper can be walked past a
one-second timeout in simulated time without the test sleeping.
*/

using namespace RakNet;

namespace {

constexpr int kMTUSize = 1492;
constexpr unsigned short kUnusedPeerPort = 60001;

// Any splitPacketId, arbitrary: nothing in the layer treats a particular value
// specially, it only has to be consistent within one message.
constexpr SplitPacketIdType kSplitPacketId = 7;

// Short enough that the reaper's simulated-time walk stays under a hundred Update
// calls; Update clamps each tick to 100 ms.
constexpr RakNet::TimeMS kTimeoutTime = 1000;

// Update clamps timeSinceLastTick to 100 ms, so anything larger is wasted motion.
constexpr CCTimeType kTickMicroseconds = 100000;

// ---------------------------------------------------------------------------
// Datagram construction
//
// Mirrors DatagramHeaderFormat::Serialize and
// ReliabilityLayer::WriteToBitStreamFromInternalPacket, which are both private to
// ReliabilityLayer.cpp. Kept field-for-field in the same order as those two so a
// wire-format change shows up here as a diff rather than as a mystery.
// ---------------------------------------------------------------------------

struct SplitChunk
{
    PacketReliability reliability = UNRELIABLE;
    MessageNumberType reliableMessageNumber = 0;
    OrderingIndexType orderingIndex = 0;
    OrderingIndexType sequencingIndex = 0;
    unsigned char orderingChannel = 0;
    SplitPacketIndexType splitPacketCount = 2;
    SplitPacketIdType splitPacketId = kSplitPacketId;
    SplitPacketIndexType splitPacketIndex = 0;
    unsigned char payload = 'A';
};

void WriteDatagramHeader( BitStream& out, DatagramSequenceNumberType datagramNumber )
{
    out.Write( true );  // isValid
    out.Write( false ); // isACK
    out.Write( false ); // isNAK
    out.Write( false ); // isPacketPair
    out.Write( false ); // isContinuousSend
    out.Write( false ); // needsBAndAs
    out.AlignWriteToByteBoundary();
#if INCLUDE_TIMESTAMP_WITH_DATAGRAMS == 1
    out.Write( (RakNet::TimeMS)0 );
#endif
    out.Write( datagramNumber );
}

void WriteSplitChunk( BitStream& out, const SplitChunk& chunk )
{
    out.AlignWriteToByteBoundary();

    unsigned char tempChar = (unsigned char)chunk.reliability;
    out.WriteBits( &tempChar, 3, true );
    out.Write( true ); // hasSplitPacket
    out.AlignWriteToByteBoundary();

    // One payload byte, the wire minimum.
    unsigned short dataBitLength = (unsigned short)BYTES_TO_BITS( 1 );
    out.WriteAlignedVar16( (const char*)&dataBitLength );

    if( chunk.reliability == RELIABLE ||
        chunk.reliability == RELIABLE_SEQUENCED ||
        chunk.reliability == RELIABLE_ORDERED )
    {
        out.Write( chunk.reliableMessageNumber );
    }
    out.AlignWriteToByteBoundary();

    if( chunk.reliability == UNRELIABLE_SEQUENCED || chunk.reliability == RELIABLE_SEQUENCED )
    {
        out.Write( chunk.sequencingIndex );
    }

    if( chunk.reliability == UNRELIABLE_SEQUENCED ||
        chunk.reliability == RELIABLE_SEQUENCED ||
        chunk.reliability == RELIABLE_ORDERED )
    {
        out.Write( chunk.orderingIndex );
        tempChar = chunk.orderingChannel;
        out.WriteAlignedVar8( (const char*)&tempChar );
    }

    out.WriteAlignedVar32( (const char*)&chunk.splitPacketCount );
    out.WriteAlignedVar16( (const char*)&chunk.splitPacketId );
    out.WriteAlignedVar32( (const char*)&chunk.splitPacketIndex );

    out.WriteAlignedBytes( &chunk.payload, 1 );
}

// ---------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------

/// A ReliabilityLayer plus the arguments its public entry points demand, so a test
/// body reads as Deliver()/Tick()/Receive() and nothing else.
class LayerUnderTest
{
public:
    explicit LayerUnderTest( RakNetSocket2* socket = nullptr )
    : m_socket( socket )
    , m_address( "127.0.0.1", kUnusedPeerPort )
    {
        m_layer.Reset( true, kMTUSize, false );
        m_layer.SetTimeoutTime( kTimeoutTime );

        // Reset stamps lastUpdateTime from the real clock, and Update ignores any time
        // at or before it. Start just past that so simulated time only moves forward.
        m_time = RakNet::GetTimeUS() + kTickMicroseconds;
    }

    /// Hand one datagram to the layer as though it had just arrived from the far side.
    bool Deliver( const BitStream& datagram )
    {
        return m_layer.HandleSocketReceiveFromConnectedPlayer(
            (const char*)datagram.GetData(), (unsigned int)datagram.GetNumberOfBytesUsed(),
            m_address, m_plugins, kMTUSize, m_socket, &m_rnr, m_time, m_updateBitStream );
    }

    /// One datagram carrying one split chunk, with the next datagram number.
    bool DeliverChunk( const SplitChunk& chunk )
    {
        BitStream datagram;
        WriteDatagramHeader( datagram, m_datagramNumber++ );
        WriteSplitChunk( datagram, chunk );
        return Deliver( datagram );
    }

    /// Advance simulated time by \a microseconds, calling Update as often as a real
    /// Peer would - Update clamps each tick, so one long jump is not the same thing.
    void Advance( CCTimeType microseconds )
    {
        for( CCTimeType elapsed = 0; elapsed < microseconds; elapsed += kTickMicroseconds )
        {
            m_time += kTickMicroseconds;
            m_layer.Update( m_socket, m_address, kMTUSize, m_time, 0, m_plugins, &m_rnr, m_updateBitStream );
        }
    }

    /// Number of bits of user message waiting, freeing whatever it dequeues.
    BitSize_t ReceiveBits()
    {
        unsigned char* data = nullptr;
        BitSize_t bits = m_layer.Receive( &data );
        if( data != nullptr )
        {
            rakFree_Ex( data, _FILE_AND_LINE_ );
        }
        return bits;
    }

    bool IsDeadConnection() { return m_layer.IsDeadConnection(); }

private:
    ReliabilityLayer m_layer;
    RakNetSocket2* m_socket;
    SystemAddress m_address;
    std::vector<PluginInterface2*> m_plugins;
    RakNetRandom m_rnr;
    BitStream m_updateBitStream;
    CCTimeType m_time = 0;
    DatagramSequenceNumberType m_datagramNumber = 0;
};

/// Watches rakMalloc_Ex while it is in scope: records the largest allocation, and
/// optionally fails one chosen size.
///
/// SetMalloc_Ex is RakNet's own documented hook, so this drives the real allocator
/// rather than a test-only seam. It is global and not thread-safe, which shapes how the
/// cases below use it: the ones that only measure run with no Peer at all, and the one
/// that injects a failure fails an *exact* byte count - a size no other allocation in
/// the process plausibly asks for - rather than everything above a threshold.
class MallocProbe
{
public:
    /// \a failSize, when non-zero, makes an allocation of exactly that many bytes return
    /// NULL rather than reaching the real allocator. Every other size passes through.
    explicit MallocProbe( size_t failSize = 0 )
    {
        RakAssert( s_previous == nullptr );
        s_largest = 0;
        s_failSize = failSize;
        s_previous = RakNet::GetMalloc_Ex();
        RakNet::SetMalloc_Ex( &Intercept );
    }

    ~MallocProbe()
    {
        RakNet::SetMalloc_Ex( s_previous );
        s_previous = nullptr;
        s_failSize = 0;
    }

    MallocProbe( const MallocProbe& ) = delete;
    MallocProbe& operator=( const MallocProbe& ) = delete;

    static size_t Largest() { return s_largest; }

private:
    static void* Intercept( size_t size, const char* file, unsigned int line )
    {
        if( size > s_largest )
        {
            s_largest = size;
        }
        if( s_failSize != 0 && size == s_failSize )
        {
            return nullptr;
        }
        return s_previous( size, file, line );
    }

    static void* ( *s_previous )( size_t, const char*, unsigned int );
    static size_t s_largest;
    static size_t s_failSize;
};

void* ( *MallocProbe::s_previous )( size_t, const char*, unsigned int ) = nullptr;
size_t MallocProbe::s_largest = 0;
size_t MallocProbe::s_failSize = 0;

// What a channel at exactly the cap costs: one pointer per chunk.
constexpr size_t kCapCost = sizeof( InternalPacket* ) * (size_t)MAXIMUM_SPLIT_PACKET_COUNT;

/// Deliver an ordinary, well-formed two-chunk split message under kSplitPacketId and
/// check it reassembles into its two payload bytes.
///
/// The workhorse of every "and the layer is still usable" assertion below. Under
/// kSplitPacketId rather than some unused id deliberately: if a preceding hostile chunk
/// had been accepted, its channel would still be open under that id, these two chunks
/// would land in it, and the message would never complete.
void CheckOrdinarySplitMessageReassembles( LayerUnderTest& layer )
{
    SplitChunk first;
    first.splitPacketCount = 2;
    first.splitPacketIndex = 0;
    first.payload = 'x';
    CHECK( layer.DeliverChunk( first ) );

    SplitChunk second = first;
    second.splitPacketIndex = 1;
    second.payload = 'y';
    CHECK( layer.DeliverChunk( second ) );

    CHECK( layer.ReceiveBits() == BYTES_TO_BITS( 2 ) );
}

} // namespace

TEST_CASE( "A split packet count at the cap allocates one pointer array and no more", "[network]" )
{
    LayerUnderTest layer;

    SplitChunk chunk;
    chunk.splitPacketCount = MAXIMUM_SPLIT_PACKET_COUNT;
    chunk.splitPacketIndex = 0;

    size_t largest = 0;
    {
        MallocProbe probe;
        CHECK( layer.DeliverChunk( chunk ) );
        largest = MallocProbe::Largest();
    }

    // Accepted: the channel was opened, so the pointer array was allocated. That it is
    // *exactly* the cap's cost is the point - nothing scaled it up.
    CHECK( largest == kCapCost );

    // Nothing is delivered from one chunk of a 65,536-chunk message.
    CHECK( layer.ReceiveBits() == 0 );
    CHECK_FALSE( layer.IsDeadConnection() );
}

TEST_CASE( "A split packet count above the cap is dropped without allocating", "[network]" )
{
    SplitChunk chunk;
    chunk.splitPacketCount = MAXIMUM_SPLIT_PACKET_COUNT + 1;
    chunk.splitPacketIndex = 0;

    SECTION( "nothing is allocated for it" )
    {
        // No Peer in this section: the probe is global, so nothing else in the process
        // may be allocating through RakNet while it is installed.
        LayerUnderTest layer;

        size_t largest = 0;
        {
            MallocProbe probe;
            CHECK( layer.DeliverChunk( chunk ) );
            largest = MallocProbe::Largest();
        }

        // One over the cap, so nothing the size of a channel's pointer array may appear.
        CHECK( largest < kCapCost );
        CHECK( layer.ReceiveBits() == 0 );
        CHECK_FALSE( layer.IsDeadConnection() );
    }

    SECTION( "it leaves no channel behind under its splitPacketId" )
    {
        // The allocation probe above cannot tell "rejected" from "allocated by some route
        // the probe does not see". This can: an accepted chunk would have opened a
        // 65,537-slot channel under kSplitPacketId, and the ordinary two-chunk message
        // that follows would land in that channel and never complete it.
        PeerScope peers;
        RakNetSocket2* socket = peers.Client()->GetSocket( UNASSIGNED_SYSTEM_ADDRESS );
        REQUIRE( socket != nullptr );

        LayerUnderTest layer( socket );

        CHECK( layer.DeliverChunk( chunk ) );

        CheckOrdinarySplitMessageReassembles( layer );
        CHECK_FALSE( layer.IsDeadConnection() );
    }
}

TEST_CASE( "An enormous split packet count is dropped rather than allocated", "[network]" )
{
    // 0x7FFFFFFF is the largest count that survives OP_NEW_ARRAY's int parameter, so it
    // is the worst case that reaches the allocator as a positive size: 16 GiB from a
    // 14-byte message. 0x80000000 is the smallest that arrives negative, where the
    // allocation throws std::bad_array_new_length instead - uncaught, so a remote kill.
    // 0xFFFFFFFF is the top of the field. All three must be refused at the same gate.
    const SplitPacketIndexType hostileCount = GENERATE( (SplitPacketIndexType)0x7FFFFFFF,
                                                        (SplitPacketIndexType)0x80000000,
                                                        (SplitPacketIndexType)0xFFFFFFFF );

    SplitChunk chunk;
    chunk.splitPacketCount = hostileCount;
    chunk.splitPacketIndex = 0;

    SECTION( "nothing is allocated for it" )
    {
        // No Peer in this section: the probe is global, so nothing else in the process
        // may be allocating through RakNet while it is installed.
        LayerUnderTest layer;

        size_t largest = 0;
        {
            MallocProbe probe;
            CHECK( layer.DeliverChunk( chunk ) );
            largest = MallocProbe::Largest();
        }

        CHECK( largest < kCapCost );
        CHECK( layer.ReceiveBits() == 0 );
        CHECK_FALSE( layer.IsDeadConnection() );
    }

    SECTION( "the layer stays up and still reassembles afterwards" )
    {
        // Completing a message acks immediately, which needs a socket. A started Peer's
        // own socket is the least ceremonious way to get one; the address the layer sends
        // to is a port nothing is listening on, so the traffic goes nowhere.
        PeerScope peers;
        RakNetSocket2* socket = peers.Client()->GetSocket( UNASSIGNED_SYSTEM_ADDRESS );
        REQUIRE( socket != nullptr );

        LayerUnderTest layer( socket );

        CHECK( layer.DeliverChunk( chunk ) );
        CHECK( layer.ReceiveBits() == 0 );

        CheckOrdinarySplitMessageReassembles( layer );
        CHECK_FALSE( layer.IsDeadConnection() );
    }
}

TEST_CASE( "A failed channel allocation drops the datagram instead of the process", "[network]" )
{
    // The cap alone is not enough: a memory-pressured process can still be killed by a
    // large-but-legal split message, because the allocation itself is a throw site.
    // ADR-0002 rules that out in Source/, so this drives the failure and asserts the
    // layer returns normally and keeps working.
    //
    // The chunk is at the cap, so its channel array is exactly kCapCost bytes, and the
    // probe fails that one exact size - not everything above a threshold, which a Peer's
    // own threads could trip.
    PeerScope peers;
    RakNetSocket2* socket = peers.Client()->GetSocket( UNASSIGNED_SYSTEM_ADDRESS );
    REQUIRE( socket != nullptr );

    LayerUnderTest layer( socket );

    SplitChunk chunk;
    chunk.splitPacketCount = MAXIMUM_SPLIT_PACKET_COUNT;
    chunk.splitPacketIndex = 0;

    {
        MallocProbe probe( kCapCost );
        CHECK( layer.DeliverChunk( chunk ) );
    }

    CHECK( layer.ReceiveBits() == 0 );
    CHECK_FALSE( layer.IsDeadConnection() );

    // No half-built channel was left under kSplitPacketId either.
    CheckOrdinarySplitMessageReassembles( layer );
    CHECK_FALSE( layer.IsDeadConnection() );
}

TEST_CASE( "A split packet channel that stalls is reaped, whatever its reliability", "[network]" )
{
    // Every reliability a split message can arrive under. RELIABLE is the one that
    // matters most: upstream's commented-out reaper skipped it, so an attacker only had
    // to set it to keep the memory.
    const PacketReliability reliability = GENERATE( UNRELIABLE, UNRELIABLE_SEQUENCED, RELIABLE,
                                                    RELIABLE_ORDERED, RELIABLE_SEQUENCED );

    // Update needs a real socket to flush acks through. A started Peer's own socket is
    // the least ceremonious way to get one; the address the layer sends to is a port
    // nothing is listening on, so the traffic goes nowhere.
    PeerScope peers;
    RakNetSocket2* socket = peers.Client()->GetSocket( UNASSIGNED_SYSTEM_ADDRESS );
    REQUIRE( socket != nullptr );

    SplitChunk first;
    first.reliability = reliability;
    first.reliableMessageNumber = 0;
    first.splitPacketCount = 2;
    first.splitPacketIndex = 0;
    first.payload = 'x';

    SplitChunk second = first;
    second.reliableMessageNumber = 1;
    second.splitPacketIndex = 1;
    second.payload = 'y';

    SECTION( "both chunks back to back reassemble" )
    {
        // The control. Without it, the reaped case below could pass because the fixture
        // never delivers anything at all.
        LayerUnderTest layer( socket );

        CHECK( layer.DeliverChunk( first ) );
        CHECK( layer.ReceiveBits() == 0 );

        CHECK( layer.DeliverChunk( second ) );
        CHECK( layer.ReceiveBits() == BYTES_TO_BITS( 2 ) );
    }

    SECTION( "the second chunk after the timeout finds nothing to complete" )
    {
        LayerUnderTest layer( socket );

        CHECK( layer.DeliverChunk( first ) );
        CHECK( layer.ReceiveBits() == 0 );

        // A stalled channel is freed within timeoutTime of its last chunk. Walk twice
        // that so the assertion is about the reaper existing, not about its cadence.
        layer.Advance( (CCTimeType)kTimeoutTime * 1000 * 2 );

        // The channel is gone, so this chunk opens a fresh one holding only itself and
        // the message never completes. Before the reaper existed the first chunk was
        // still there and this delivered two bytes.
        CHECK( layer.DeliverChunk( second ) );
        CHECK( layer.ReceiveBits() == 0 );

        CHECK_FALSE( layer.IsDeadConnection() );
    }
}
