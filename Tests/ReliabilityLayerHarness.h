#pragma once

#include "BitStream.h"
#include "GetTime.h"
#include "RakMemoryOverride.h"
#include "RakNetSocket2.h"
#include "ReliabilityLayer.h"

#include <vector>

/*
Drives a ReliabilityLayer with hand-built datagrams, the way a hostile System would
rather than the way RakNet's own sender does. Shared by the tests that need to send the
layer something a Peer never would.

Datagrams are built by hand because there is no supported way to ask a Peer to send a
malformed one. ReliabilityLayer is driven directly: HandleSocketReceiveFromConnectedPlayer,
Update and Receive are all public, and Update takes the current time as a parameter, so a
timer can be walked past in simulated time without the test sleeping.
*/

namespace ReliabilityLayerHarness {

using namespace RakNet;

constexpr int kMTUSize = 1492;
constexpr unsigned short kUnusedPeerPort = 60001;

// Any splitPacketId, arbitrary: nothing in the layer treats a particular value
// specially, it only has to be consistent within one message.
constexpr SplitPacketIdType kSplitPacketId = 7;

// Short enough that a reaper's simulated-time walk stays under a hundred Update calls;
// Update clamps each tick to 100 ms.
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

/// One message as it goes on the wire. By default a chunk of a two-chunk split message
/// carrying one byte, the wire minimum.
struct WireMessage
{
    PacketReliability reliability = UNRELIABLE;
    MessageNumberType reliableMessageNumber = 0;
    OrderingIndexType orderingIndex = 0;
    OrderingIndexType sequencingIndex = 0;
    unsigned char orderingChannel = 0;
    bool isSplit = true;
    SplitPacketIndexType splitPacketCount = 2;
    SplitPacketIdType splitPacketId = kSplitPacketId;
    SplitPacketIndexType splitPacketIndex = 0;
    /// Repeated payloadBytes times, unless payloadData is set.
    unsigned char payload = 'A';
    unsigned short payloadBytes = 1;
    /// payloadBytes bytes to send instead of repeating payload. Not owned.
    const unsigned char* payloadData = nullptr;
};

inline void WriteDatagramHeader( BitStream& out, DatagramSequenceNumberType datagramNumber )
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

inline void WriteWireMessage( BitStream& out, const WireMessage& message )
{
    out.AlignWriteToByteBoundary();

    unsigned char tempChar = (unsigned char)message.reliability;
    out.WriteBits( &tempChar, 3, true );
    out.Write( message.isSplit ); // hasSplitPacket
    out.AlignWriteToByteBoundary();

    unsigned short dataBitLength = (unsigned short)BYTES_TO_BITS( message.payloadBytes );
    out.WriteAlignedVar16( (const char*)&dataBitLength );

    if( message.reliability == RELIABLE ||
        message.reliability == RELIABLE_SEQUENCED ||
        message.reliability == RELIABLE_ORDERED )
    {
        out.Write( message.reliableMessageNumber );
    }
    out.AlignWriteToByteBoundary();

    if( message.reliability == UNRELIABLE_SEQUENCED || message.reliability == RELIABLE_SEQUENCED )
    {
        out.Write( message.sequencingIndex );
    }

    if( message.reliability == UNRELIABLE_SEQUENCED ||
        message.reliability == RELIABLE_SEQUENCED ||
        message.reliability == RELIABLE_ORDERED )
    {
        out.Write( message.orderingIndex );
        tempChar = message.orderingChannel;
        out.WriteAlignedVar8( (const char*)&tempChar );
    }

    if( message.isSplit )
    {
        out.WriteAlignedVar32( (const char*)&message.splitPacketCount );
        out.WriteAlignedVar16( (const char*)&message.splitPacketId );
        out.WriteAlignedVar32( (const char*)&message.splitPacketIndex );
    }

    if( message.payloadData != nullptr )
    {
        out.WriteAlignedBytes( message.payloadData, message.payloadBytes );
        return;
    }
    for( unsigned short i = 0; i < message.payloadBytes; i++ )
    {
        out.WriteAlignedBytes( &message.payload, 1 );
    }
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
        ResetForReuse();

        // Reset stamps lastUpdateTime from the real clock, and Update ignores any time
        // at or before it. Start just past that so simulated time only moves forward.
        m_time = RakNet::GetTimeUS() + kTickMicroseconds;
    }

    /// Hand one datagram to the layer as though it had just arrived from the far side.
    bool Deliver( const BitStream& datagram )
    {
        return m_layer.HandleSocketReceiveFromConnectedPlayer(
            (const char*)datagram.GetData(), (unsigned int)datagram.GetNumberOfBytesUsed(),
            m_address, m_plugins, kMTUSize, m_socket, m_time, m_updateBitStream );
    }

    /// One datagram carrying \a messages, with the next datagram number.
    bool DeliverMessages( const std::vector<WireMessage>& messages )
    {
        BitStream datagram;
        WriteDatagramHeader( datagram, m_datagramNumber++ );
        for( const WireMessage& message : messages )
        {
            WriteWireMessage( datagram, message );
        }
        return Deliver( datagram );
    }

    /// One datagram carrying one message, with the next datagram number.
    bool DeliverChunk( const WireMessage& chunk )
    {
        return DeliverMessages( { chunk } );
    }

    /// Advance simulated time by \a microseconds, calling Update as often as a real
    /// Peer would - Update clamps each tick, so one long jump is not the same thing.
    void Advance( CCTimeType microseconds )
    {
        for( CCTimeType elapsed = 0; elapsed < microseconds; elapsed += kTickMicroseconds )
        {
            m_time += kTickMicroseconds;
            m_layer.Update( m_socket, m_address, kMTUSize, m_time, 0, m_plugins, m_updateBitStream );
        }
    }

    /// One Update at the real clock's time, for the few timers the layer reads from the
    /// real clock rather than from its time parameter. Simulated time is not wound back.
    void UpdateNow()
    {
        const CCTimeType now = RakNet::GetTimeUS();
        if( now > m_time )
        {
            m_time = now;
        }
        m_layer.Update( m_socket, m_address, kMTUSize, m_time, 0, m_plugins, m_updateBitStream );
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

    /// What RakPeer::SetSplitMessageProgressInterval pushes into each layer. Deliberately
    /// not called by the constructor: the default the layer starts with is what several
    /// cases are about.
    void SetProgressInterval( int interval ) { m_layer.SetSplitMessageProgressInterval( interval ); }

    /// Put the layer back the way the constructor found it, the way RakPeer does when a
    /// System's slot in remoteSystemList is handed to the next connection.
    void ResetForReuse()
    {
        m_layer.Reset( true, kMTUSize, false );
        m_layer.SetTimeoutTime( kTimeoutTime );
        m_datagramNumber = 0;
    }

    /// The layer itself, for what the wrappers above do not cover.
    ReliabilityLayer& Layer() { return m_layer; }

private:
    ReliabilityLayer m_layer;
    RakNetSocket2* m_socket;
    SystemAddress m_address;
    std::vector<PluginInterface2*> m_plugins;
    BitStream m_updateBitStream;
    CCTimeType m_time = 0;
    DatagramSequenceNumberType m_datagramNumber = 0;
};

} // namespace ReliabilityLayerHarness
