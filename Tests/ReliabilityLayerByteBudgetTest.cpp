#include "PeerScope.h"
#include "RawSystem.h"
#include "ReliabilityLayerHarness.h"

#include "GetTime.h"
#include "MTUSize.h"
#include "MessageIdentifiers.h"
#include "RakNetStatistics.h"
#include "RakPeerInterface.h"
#include "ReliabilityLayer.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

/*
Pins the reliability layer's byte budgets (ADR-0005).

Before them, two things a System sends grew without limit. The number of live split
channels was bounded only by the 16-bit splitPacketId keyspace, and one chunk per channel
every few seconds kept all 65,536 alive past the stall reaper, each holding a pointer array
of up to 512 KiB. And any ordered or sequenced message ahead of the read index went into an
ordering heap with no limit at all, freed only when the connection reset. A Half-open System
reached both, since RakPeer only checks what it sends after reassembly and ordering.

The shapes pinned here:

- A Half-open System's split chunks and ordered or sequenced messages are dropped before
  either could hold them.
- Over the per-connection budget, unreliable data is dropped and the connection stays open
  (silent drop); reliable data closes the connection (byte budget -> close). Both for split
  channels and for an ordering heap filled across a hole.
- Over the Peer-wide budget, the connection holding the most is closed, and a lighter
  connection's large Message in flight survives.
- End to end through a Peer: the close is reported as ID_CONNECTION_LOST and the System is
  sent ID_DISCONNECTION_NOTIFICATION; a Half-open System's chunks cost the Peer nothing.

Most cases drive a ReliabilityLayer directly with hand-built datagrams, as
SplitPacketReassemblyTest does. The two end-to-end cases need to be a System, not drive a
Peer, and use RawSystem.
*/

using namespace RakNet;
using namespace ReliabilityLayerHarness;

namespace {

// What the layer charges for one chunk carrying one byte, and for a split channel sized at
// the cap, as ReliabilityLayer::HeldPacketCost and EmptySplitPacketChannelCost define them.
constexpr uint64_t kOneByteChunkCost = sizeof( InternalPacket ) + 1;
constexpr uint64_t kWidestChannelCost = sizeof( SplitPacketChannel ) + sizeof( InternalPacket* ) * (uint64_t)MAXIMUM_SPLIT_PACKET_COUNT;

// A chunk of a split message sized at the cap, the most expensive channel one datagram can
// open, under its own splitPacketId.
WireMessage WidestChannelChunk( SplitPacketIdType splitPacketId, PacketReliability reliability, MessageNumberType reliableMessageNumber )
{
    WireMessage chunk;
    chunk.reliability = reliability;
    chunk.reliableMessageNumber = reliableMessageNumber;
    chunk.splitPacketCount = MAXIMUM_SPLIT_PACKET_COUNT;
    chunk.splitPacketId = splitPacketId;
    chunk.splitPacketIndex = 0;
    return chunk;
}

WireMessage UnsplitMessage( PacketReliability reliability, MessageNumberType reliableMessageNumber )
{
    WireMessage message;
    message.isSplit = false;
    message.reliability = reliability;
    message.reliableMessageNumber = reliableMessageNumber;
    return message;
}

RakNetStatistics StatisticsOf( LayerUnderTest& layer )
{
    RakNetStatistics statistics;
    layer.Layer().GetStatistics( &statistics );
    return statistics;
}

// Enough widest channels to pass the default connection budget with room to spare: it
// admits about 129.
constexpr unsigned int kMoreChannelsThanTheDefaultBudget = 300;

} // namespace

TEST_CASE( "A conforming Message of MAXIMUM_MESSAGE_SIZE fits the default connection byte budget", "[network]" )
{
    // At the lowest MTU a largest Message arrives as MAXIMUM_SPLIT_PACKET_COUNT chunks, and
    // while it is reassembled costs its data, one record per chunk and the widest pointer
    // array. RakNetDefines.h derives the default from this; the check keeps the derivation
    // honest if InternalPacket grows. MaximumMessageSizeTest sends one end to end.
    const uint64_t largestMessageCost =
        (uint64_t)MAXIMUM_MESSAGE_SIZE + sizeof( InternalPacket ) * (uint64_t)MAXIMUM_SPLIT_PACKET_COUNT + kWidestChannelCost;

    CHECK( largestMessageCost <= RELIABILITY_LAYER_CONNECTION_BYTE_BUDGET );
    CHECK( RELIABILITY_LAYER_CONNECTION_BYTE_BUDGET <= RELIABILITY_LAYER_PEER_BYTE_BUDGET );
}

TEST_CASE( "A Half-open System's split chunks and ordered messages never reach reassembly or ordering", "[network]" )
{
    LayerUnderTest layer;

    // Ahead of the read index on its channel, so with the gate off it goes into an
    // ordering heap and stays there.
    WireMessage aheadOfTheReadIndex = UnsplitMessage( RELIABLE_ORDERED, 0 );
    aheadOfTheReadIndex.orderingIndex = 3;

    SECTION( "while the System is Half-open" )
    {
        layer.Layer().SetHalfOpen( true );

        CHECK( layer.DeliverChunk( WidestChannelChunk( kSplitPacketId, UNRELIABLE, 0 ) ) );
        CHECK( layer.Layer().GetBytesHeld() == 0 );

        CHECK( layer.DeliverChunk( aheadOfTheReadIndex ) );
        CHECK( layer.Layer().GetBytesHeld() == 0 );
        CHECK( layer.ReceiveBits() == 0 );

        // Not only buffering is refused: a sequenced message at the read index would be
        // delivered straight away, and is still not something a Half-open System sends.
        CHECK( layer.DeliverChunk( UnsplitMessage( UNRELIABLE_SEQUENCED, 0 ) ) );
        CHECK( layer.ReceiveBits() == 0 );

        // An unsplit, unordered message is passed up for RakPeer to judge - its
        // connection request, or nonsense that gets the address banned.
        CHECK( layer.DeliverChunk( UnsplitMessage( RELIABLE, 1 ) ) );
        CHECK( layer.ReceiveBits() == BYTES_TO_BITS( 1 ) );

        CHECK( StatisticsOf( layer ).bytesHeldForReassemblyAndOrdering == 0 );
        CHECK_FALSE( layer.Layer().TakeClosedOverBudget() );
    }

    SECTION( "and the same messages once it is not" )
    {
        // The contrast that shows the gate, not something else, is what held nothing above.
        CHECK( layer.DeliverChunk( WidestChannelChunk( kSplitPacketId, UNRELIABLE, 0 ) ) );
        CHECK( layer.Layer().GetBytesHeld() == kWidestChannelCost + kOneByteChunkCost );

        CHECK( layer.DeliverChunk( aheadOfTheReadIndex ) );
        CHECK( layer.Layer().GetBytesHeld() > kWidestChannelCost + kOneByteChunkCost );
        CHECK( StatisticsOf( layer ).bytesHeldForReassemblyAndOrdering == layer.Layer().GetBytesHeld() );
    }
}

TEST_CASE( "Split channels kept alive past the connection byte budget are stopped at it", "[network]" )
{
    // Each chunk opens a fresh channel sized at the cap - one pointer array of 512 KiB for a
    // one-byte datagram. At the default budget, without which 65,536 of these are 32 GiB.
    ReliabilityBufferBudget peerBudget;
    LayerUnderTest layer;
    peerBudget.Attach( &layer.Layer() );

    SECTION( "unreliable chunks are dropped, and the connection stays open" )
    {
        unsigned int opened = 0;
        while( opened < kMoreChannelsThanTheDefaultBudget && StatisticsOf( layer ).messagesDroppedOverConnectionBudget == 0 )
        {
            CHECK( layer.DeliverChunk( WidestChannelChunk( (SplitPacketIdType)opened, UNRELIABLE, 0 ) ) );
            ++opened;
        }

        const uint64_t held = layer.Layer().GetBytesHeld();
        CHECK( StatisticsOf( layer ).messagesDroppedOverConnectionBudget == 1 );
        CHECK( held <= RELIABILITY_LAYER_CONNECTION_BYTE_BUDGET );
        CHECK( held + kWidestChannelCost + kOneByteChunkCost > RELIABILITY_LAYER_CONNECTION_BYTE_BUDGET );
        CHECK( held == ( opened - 1 ) * ( kWidestChannelCost + kOneByteChunkCost ) );

        CHECK_FALSE( layer.Layer().TakeClosedOverBudget() );
        CHECK( peerBudget.GetConnectionBudgetCloses() == 0 );

        // Still usable: something that needs no buffering is delivered as ever.
        CHECK( layer.DeliverChunk( UnsplitMessage( UNRELIABLE, 0 ) ) );
        CHECK( layer.ReceiveBits() == BYTES_TO_BITS( 1 ) );
    }

    SECTION( "reliable chunks close the connection" )
    {
        // Each of these was acknowledged when it arrived, so it cannot be dropped: the
        // sender will not send it again.
        unsigned int opened = 0;
        while( opened < kMoreChannelsThanTheDefaultBudget && layer.Layer().GetBytesHeld() + kWidestChannelCost + kOneByteChunkCost <= RELIABILITY_LAYER_CONNECTION_BYTE_BUDGET )
        {
            CHECK( layer.DeliverChunk( WidestChannelChunk( (SplitPacketIdType)opened, RELIABLE, opened ) ) );
            ++opened;
        }
        CHECK_FALSE( layer.Layer().TakeClosedOverBudget() );

        CHECK( layer.DeliverChunk( WidestChannelChunk( (SplitPacketIdType)opened, RELIABLE, opened ) ) );

        CHECK( layer.Layer().TakeClosedOverBudget() );
        CHECK_FALSE( layer.Layer().TakeClosedOverBudget() ); // Reported once
        CHECK( layer.Layer().GetBytesHeld() == 0 );          // and everything freed
        CHECK( peerBudget.GetBytesHeld() == 0 );
        CHECK( StatisticsOf( layer ).messagesDroppedOverConnectionBudget == 0 );
        CHECK( StatisticsOf( layer ).connectionsClosedOverConnectionBudget == 1 );
        CHECK( StatisticsOf( layer ).connectionsClosedOverPeerBudget == 0 );

        // Nothing the System sends from here on is held or delivered.
        CHECK( layer.DeliverChunk( WidestChannelChunk( 60000, RELIABLE, opened + 1 ) ) );
        CHECK( layer.DeliverChunk( UnsplitMessage( RELIABLE, opened + 2 ) ) );
        CHECK( layer.Layer().GetBytesHeld() == 0 );
        CHECK( layer.ReceiveBits() == 0 );
    }
}

TEST_CASE( "An ordering heap filled across a hole is stopped at the connection byte budget", "[network]" )
{
    // A small budget, so the heap fills in a few hundred datagrams rather than a few
    // hundred thousand. The charge per message is the same at any budget.
    constexpr uint64_t kBudget = 64 * 1024;
    constexpr unsigned int kEnoughMessages = 1000;

    LayerUnderTest layer;
    layer.Layer().SetConnectionByteBudget( kBudget );

    SECTION( "reliable ordered messages close the connection" )
    {
        // Ordering index 0 never arrives, so every one of these waits behind it.
        unsigned int sent = 0;
        while( sent < kEnoughMessages && !layer.Layer().TakeClosedOverBudget() )
        {
            WireMessage message = UnsplitMessage( RELIABLE_ORDERED, sent );
            message.orderingIndex = sent + 1;
            message.payloadBytes = 200;
            CHECK( layer.DeliverChunk( message ) );
            CHECK( layer.Layer().GetBytesHeld() <= kBudget );
            ++sent;
        }

        CHECK( sent < kEnoughMessages );
        CHECK( layer.Layer().GetBytesHeld() == 0 );
        CHECK( layer.ReceiveBits() == 0 );
    }

    SECTION( "unreliable sequenced messages are dropped, and the hole still fills" )
    {
        // Sequenced on ordering index 1 while the read index is 0: buffered until an ordered
        // message at index 0 arrives.
        unsigned int sent = 0;
        while( sent < kEnoughMessages && StatisticsOf( layer ).messagesDroppedOverConnectionBudget == 0 )
        {
            WireMessage message = UnsplitMessage( UNRELIABLE_SEQUENCED, 0 );
            message.orderingIndex = 1;
            message.sequencingIndex = sent;
            message.payloadBytes = 200;
            CHECK( layer.DeliverChunk( message ) );
            ++sent;
        }

        CHECK( sent < kEnoughMessages );
        CHECK( layer.Layer().GetBytesHeld() <= kBudget );
        CHECK_FALSE( layer.Layer().TakeClosedOverBudget() );
        CHECK( layer.ReceiveBits() == 0 );

        // The ordered message the heap was waiting for releases everything it held.
        CHECK( layer.DeliverChunk( UnsplitMessage( RELIABLE_ORDERED, 0 ) ) );
        CHECK( layer.Layer().GetBytesHeld() == 0 );

        unsigned int delivered = 0;
        while( layer.ReceiveBits() != 0 )
            ++delivered;
        CHECK( delivered == sent ); // The ordered one, and all but the one dropped
    }
}

TEST_CASE( "What a stalled split channel held is released when it is reaped", "[network]" )
{
    // Charges are released wherever a channel is freed, not only on reassembly and close. A
    // reaped channel that kept its charge would shrink the budget for good.
    //
    // Update acks what arrived, which needs a socket.
    PeerScope peers;
    RakNetSocket2* socket = peers.Client()->GetSocket( UNASSIGNED_SYSTEM_ADDRESS );
    REQUIRE( socket != nullptr );

    ReliabilityBufferBudget peerBudget;
    LayerUnderTest layer( socket );
    peerBudget.Attach( &layer.Layer() );

    CHECK( layer.DeliverChunk( WidestChannelChunk( kSplitPacketId, RELIABLE, 0 ) ) );
    CHECK( layer.Layer().GetBytesHeld() == kWidestChannelCost + kOneByteChunkCost );
    CHECK( peerBudget.GetBytesHeld() == layer.Layer().GetBytesHeld() );

    layer.Advance( (CCTimeType)kTimeoutTime * 1000 * 2 );

    CHECK( layer.Layer().GetBytesHeld() == 0 );
    CHECK( peerBudget.GetBytesHeld() == 0 );
}

TEST_CASE( "A System closed at its byte budget cannot keep the connection alive by sending", "[network]" )
{
    // After a close the connection lasts until the System acknowledges its disconnection
    // notification or the ack timeout gives up. That timeout runs from the last datagram to
    // arrive, so a System that kept sending without acknowledging would never reach it. Real
    // time rather than simulated: the layer stamps arrivals from the real clock.
    PeerScope peers;
    RakNetSocket2* socket = peers.Client()->GetSocket( UNASSIGNED_SYSTEM_ADDRESS );
    REQUIRE( socket != nullptr );

    LayerUnderTest layer( socket );
    layer.Layer().SetConnectionByteBudget( 1 ); // Anything held closes it

    WireMessage aheadOfTheReadIndex = UnsplitMessage( RELIABLE_ORDERED, 0 );
    aheadOfTheReadIndex.orderingIndex = 1;
    CHECK( layer.DeliverChunk( aheadOfTheReadIndex ) );
    REQUIRE( layer.Layer().TakeClosedOverBudget() );

    // What RakPeer sends next, and what this System will never acknowledge.
    unsigned char notification = (unsigned char)ID_DISCONNECTION_NOTIFICATION;
    REQUIRE( layer.Layer().Send( (char*)&notification, BYTES_TO_BITS( 1 ), LOW_PRIORITY, RELIABLE_ORDERED, 0, true, kMTUSize, RakNet::GetTimeUS(), 0 ) );

    const RakNet::TimeMS deadline = RakNet::GetTimeMS() + kTimeoutTime * 3;
    while( !layer.IsDeadConnection() && RakNet::GetTimeMS() < deadline )
    {
        layer.DeliverChunk( UnsplitMessage( UNRELIABLE, 0 ) );
        layer.UpdateNow();
        std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
    }

    CHECK( layer.IsDeadConnection() );
}

TEST_CASE( "At the Peer-wide byte budget the heaviest connection is closed, not the latest", "[network]" )
{
    // Room for four widest channels and a little more, but not five.
    constexpr uint64_t kPeerBudget = 4 * ( kWidestChannelCost + kOneByteChunkCost ) + kWidestChannelCost / 2;

    // Completing a message acks, which needs a socket.
    PeerScope peers;
    RakNetSocket2* socket = peers.Client()->GetSocket( UNASSIGNED_SYSTEM_ADDRESS );
    REQUIRE( socket != nullptr );

    // Declared first, so the layers are destroyed - and release what they hold - while it
    // still exists.
    ReliabilityBufferBudget peerBudget( kPeerBudget );
    LayerUnderTest heavy( socket );
    LayerUnderTest light( socket );
    peerBudget.Attach( &heavy.Layer() );
    peerBudget.Attach( &light.Layer() );

    SECTION( "when a lighter connection asks, the heaviest one goes" )
    {
        for( SplitPacketIdType id = 0; id < 3; id++ )
            CHECK( heavy.DeliverChunk( WidestChannelChunk( id, UNRELIABLE, 0 ) ) );

        // A large Message in flight on the light connection: two of its three chunks.
        WireMessage inFlight;
        inFlight.reliability = RELIABLE;
        inFlight.splitPacketCount = 3;
        inFlight.splitPacketId = 100;
        for( SplitPacketIndexType index = 0; index < 2; index++ )
        {
            inFlight.splitPacketIndex = index;
            inFlight.reliableMessageNumber = index;
            CHECK( light.DeliverChunk( inFlight ) );
        }

        // The light connection's next channel is what takes the total over, but it holds
        // far less than the heavy one.
        CHECK( light.DeliverChunk( WidestChannelChunk( 200, RELIABLE, 2 ) ) );
        CHECK( light.DeliverChunk( WidestChannelChunk( 201, RELIABLE, 3 ) ) );

        CHECK( heavy.Layer().TakeClosedOverBudget() );
        CHECK( heavy.Layer().GetBytesHeld() == 0 );
        CHECK_FALSE( light.Layer().TakeClosedOverBudget() );
        CHECK( peerBudget.GetPeerBudgetCloses() == 1 );
        CHECK( peerBudget.GetConnectionBudgetCloses() == 0 );
        CHECK( peerBudget.GetBytesHeld() == light.Layer().GetBytesHeld() );
        CHECK( StatisticsOf( light ).connectionsClosedOverPeerBudget == 1 );

        // The light connection's Message was not disturbed, and completes.
        inFlight.splitPacketIndex = 2;
        inFlight.reliableMessageNumber = 4;
        CHECK( light.DeliverChunk( inFlight ) );
        CHECK( light.ReceiveBits() == BYTES_TO_BITS( 3 ) );
    }

    SECTION( "when the connection asking is the heaviest, it is the one closed" )
    {
        CHECK( light.DeliverChunk( WidestChannelChunk( 0, UNRELIABLE, 0 ) ) );
        const uint64_t lightHeld = light.Layer().GetBytesHeld();

        for( SplitPacketIdType id = 0; id < 4; id++ )
            CHECK( heavy.DeliverChunk( WidestChannelChunk( id, UNRELIABLE, 0 ) ) );

        CHECK( heavy.Layer().TakeClosedOverBudget() );
        CHECK_FALSE( light.Layer().TakeClosedOverBudget() );
        CHECK( light.Layer().GetBytesHeld() == lightHeld );
        CHECK( peerBudget.GetPeerBudgetCloses() == 1 );
    }
}

TEST_CASE( "A connected System over its byte budget is reported lost and told so", "[network]" )
{
    using namespace RawSystemHarness;

    constexpr unsigned short kServerPort = 30000;
    constexpr uint64_t kRawSystemGuid = 0x00ABCDEF12345679ull;
    constexpr RakNet::TimeMS kBudgetMs = 5000;

    WinsockFixture winsock;
    PeerScope peers;
    RakPeerInterface* server = peers.Server( kServerPort );

    RawSystem rawSystem( SystemAddress( "127.0.0.1", kServerPort ), kRawSystemGuid );
    rawSystem.CompleteConnection();

    bool connected = false;
    for( RakNet::TimeMS deadline = RakNet::GetTimeMS() + kBudgetMs; !connected && RakNet::GetTimeMS() < deadline; )
    {
        for( Packet* packet = server->Receive(); packet != nullptr; server->DeallocatePacket( packet ), packet = server->Receive() )
            connected = connected || packet->data[0] == ID_NEW_INCOMING_CONNECTION;
        std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
    }
    REQUIRE( connected );

    // Reliable chunks, each opening a widest channel, until the default budget is passed.
    // Everything the System sent before was unreliable, so its reliable message numbers
    // start at 0.
    for( unsigned int i = 0; i < kMoreChannelsThanTheDefaultBudget; i++ )
        rawSystem.SendMessages( { WidestChannelChunk( (SplitPacketIdType)i, RELIABLE, i ) } );

    // Counted past the first report, and past the moment the notification goes out, so a
    // second report of the same close - from the dead-connection path, say - would show.
    unsigned int lostReports = 0;
    const RakNet::TimeMS settle = 1000;
    for( RakNet::TimeMS deadline = RakNet::GetTimeMS() + kBudgetMs; RakNet::GetTimeMS() < deadline; )
    {
        for( Packet* packet = server->Receive(); packet != nullptr; server->DeallocatePacket( packet ), packet = server->Receive() )
        {
            if( packet->data[0] == ID_CONNECTION_LOST || packet->data[0] == ID_DISCONNECTION_NOTIFICATION )
            {
                if( lostReports++ == 0 )
                    deadline = RakNet::GetTimeMS() + settle;
                CHECK( packet->data[0] == ID_CONNECTION_LOST );
            }
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
    }
    CHECK( lostReports == 1 );

    char datagram[MAXIMUM_MTU_SIZE];
    int datagramLength = 0;
    CHECK( rawSystem.WaitForMessage( ID_DISCONNECTION_NOTIFICATION, RawSystem::Framing::Connected, kBudgetMs, datagram, datagramLength ) );
}

TEST_CASE( "A Half-open System's split chunks cost a Peer nothing", "[network]" )
{
    using namespace RawSystemHarness;

    constexpr unsigned short kServerPort = 30000;
    constexpr uint64_t kRawSystemGuid = 0x00ABCDEF1234567Aull;
    constexpr unsigned int kChunks = 20;

    WinsockFixture winsock;
    PeerScope peers;
    RakPeerInterface* server = peers.Server( kServerPort );

    RawSystem rawSystem( SystemAddress( "127.0.0.1", kServerPort ), kRawSystemGuid );
    rawSystem.CompleteOfflineHandshake();

    for( unsigned int i = 0; i < kChunks; i++ )
        rawSystem.SendMessages( { WidestChannelChunk( (SplitPacketIdType)i, UNRELIABLE, 0 ) } );

    // Wait until the Peer has read every chunk, so "nothing held" is not "nothing read yet".
    // Every one is its own datagram of the same length, and nothing else this System sent
    // went through the reliability layer.
    BitStream sample;
    WriteDatagramHeader( sample, 0 );
    WriteWireMessage( sample, WidestChannelChunk( 0, UNRELIABLE, 0 ) );
    const uint64_t bytesSent = (uint64_t)kChunks * sample.GetNumberOfBytesUsed();

    // The Peer sends ID_OPEN_CONNECTION_REPLY_2 during an update cycle and publishes the
    // System's record only at the cycle's end, so the reply can arrive before the record.
    SystemAddress rawAddress = UNASSIGNED_SYSTEM_ADDRESS;
    for( RakNet::TimeMS deadline = RakNet::GetTimeMS() + 5000; rawAddress == UNASSIGNED_SYSTEM_ADDRESS && RakNet::GetTimeMS() < deadline; std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) ) )
        rawAddress = server->GetSystemAddressFromGuid( RakNetGUID( kRawSystemGuid ) );
    REQUIRE( rawAddress != UNASSIGNED_SYSTEM_ADDRESS );

    RakNetStatistics statistics;
    bool readThemAll = false;
    for( RakNet::TimeMS deadline = RakNet::GetTimeMS() + 5000; !readThemAll && RakNet::GetTimeMS() < deadline; std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) ) )
    {
        REQUIRE( server->GetStatistics( rawAddress, &statistics ) != nullptr );
        readThemAll = statistics.runningTotal[ACTUAL_BYTES_RECEIVED] >= bytesSent;
    }
    REQUIRE( readThemAll );

    // Without the gate, twenty widest channels: ten megabytes.
    CHECK( statistics.bytesHeldForReassemblyAndOrdering == 0 );
}
