#include "ConnectionWaits.h"
#include "GetTime.h"
#include "LoopbackTCP.h"
#include "PacketizedTCP.h"
#include "RakNetTypes.h"
#include "SocketDefines.h"
#include "SocketIncludes.h"
#include "TCPInterface.h"
#include "WinsockScope.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

/*
Pins what a TCP client can make TCPInterface and PacketizedTCP hold (ADR-0005).

- Incoming bytes per client. A client used to be read as fast as it sent, into a queue only
  the application drains, so an application that polled slowly held everything a client
  sent. At the cap the receive thread stops reading that client's socket, so TCP flow
  control stalls the sender and nothing is dropped.
- PacketizedTCP's message length. A 32-bit header made a connection buffer up to 4 GiB
  waiting for the rest of one message. A header announcing more than the maximum now closes
  the connection, since the stream cannot be re-framed after skipping a frame.
- Outgoing bytes per client. A client that never reads made every Send buffer forever. At
  the cap the connection is closed and reported lost.
- A reconnect from the same address before the old connection's lost event was processed.
  PacketizedTCP processes new events before lost ones, so the reconnect's entry failed to
  insert - leaking its ByteQueue - and the old connection's lost event then deleted the
  entry the reconnect was using, dropping everything it sent.

The clients that must misbehave are raw sockets, so they can stop reading or reconnect from
a fixed port. They close abortively - SO_LINGER with a zero timeout sends a RST - which
leaves no TIME_WAIT behind and lets a reconnect bind the same port again.
*/

using namespace RakNet;

namespace {

// TCP, past every port another TCPInterface test takes. CreateListenSocket does not set
// SO_REUSEADDR before it binds, so no port is shared between cases.
constexpr unsigned short kOverlongListenPort = 31050;
constexpr unsigned short kExactMaximumListenPort = 31051;
constexpr unsigned short kBackpressureListenPort = 31052;
constexpr unsigned short kNeverReadsListenPort = 31053;
constexpr unsigned short kReconnectListenPort = 31054;
constexpr unsigned short kReconnectClientPort = 31055;

// The raw clients' SO_SNDBUF and SO_RCVBUF in the cap cases, so the sender stalls and the
// never-reading client's buffer fills after little data.
constexpr int kSmallClientBuffers = 16 * 1024;

// connections is protected; the reconnect case needs its size.
class InspectablePacketizedTCP : public PacketizedTCP
{
public:
    size_t ConnectionEntryCount() const { return connections.size(); }
};

// The byte at \a offset of the stream the backpressure case sends, so a byte dropped,
// repeated or reordered anywhere shows up as a mismatch.
unsigned char PatternByte( size_t offset )
{
    return (unsigned char)( ( offset * 131 ) ^ ( offset >> 13 ) );
}

// PacketizedTCP's framing: a 32-bit length in network order, then the message.
std::vector<char> Frame( const std::vector<char>& message )
{
    const uint32_t length = htonl( (uint32_t)message.size() );
    std::vector<char> framed( sizeof( length ) );
    memcpy( framed.data(), &length, sizeof( length ) );
    framed.insert( framed.end(), message.begin(), message.end() );
    return framed;
}

} // namespace

TEST_CASE( "PacketizedTCP closes a sender announcing a message longer than the maximum", "[packetizedtcp][network]" )
{
    PacketizedTCP server;
    REQUIRE( server.Start( kOverlongListenPort, 4 ) );
    server.SetMaxMessageLength( 1024 );

    PacketizedTCP client;
    REQUIRE( client.Start( 0, 0, 1 ) );
    const SystemAddress serverAddress = client.Connect( "127.0.0.1", kOverlongListenPort, true, AF_INET );
    REQUIRE( serverAddress != UNASSIGNED_SYSTEM_ADDRESS );

    SystemAddress clientAddress = UNASSIGNED_SYSTEM_ADDRESS;
    REQUIRE( ConnectionWaits::WaitUntil( [&] { return ( clientAddress = server.HasNewIncomingConnection() ) != UNASSIGNED_SYSTEM_ADDRESS; }, LoopbackTCP::kWaitBudget ) );

    const std::vector<char> message( 1025, 'x' );
    client.Send( message.data(), (unsigned int)message.size(), serverAddress, false );

    // The header alone decides it: the server closes before the message could complete,
    // reports the loss to its application, and delivers nothing.
    SystemAddress lostAddress = UNASSIGNED_SYSTEM_ADDRESS;
    CHECK( ConnectionWaits::WaitUntil(
        [&] {
            Packet* packet = server.Receive();
            const bool isDelivered = packet != 0;
            server.DeallocatePacket( packet );
            REQUIRE_FALSE( isDelivered );
            return ( lostAddress = server.HasLostConnection() ) != UNASSIGNED_SYSTEM_ADDRESS;
        },
        LoopbackTCP::kWaitBudget ) );
    CHECK( lostAddress == clientAddress );
    CHECK( server.GetMessageLengthCapCloseCount() == 1 );

    // And the client sees its connection end.
    CHECK( ConnectionWaits::WaitUntil( [&] { return client.HasLostConnection() != UNASSIGNED_SYSTEM_ADDRESS; }, LoopbackTCP::kWaitBudget ) );

    client.Stop();
    server.Stop();
}

TEST_CASE( "PacketizedTCP delivers a message of exactly the maximum length", "[packetizedtcp][network]" )
{
    PacketizedTCP server;
    REQUIRE( server.Start( kExactMaximumListenPort, 4 ) );
    server.SetMaxMessageLength( 1024 );
    CHECK( server.GetMaxMessageLength() == 1024 );

    PacketizedTCP client;
    REQUIRE( client.Start( 0, 0, 1 ) );
    const SystemAddress serverAddress = client.Connect( "127.0.0.1", kExactMaximumListenPort, true, AF_INET );
    REQUIRE( serverAddress != UNASSIGNED_SYSTEM_ADDRESS );
    REQUIRE( ConnectionWaits::WaitUntil( [&] { return server.HasNewIncomingConnection() != UNASSIGNED_SYSTEM_ADDRESS; }, LoopbackTCP::kWaitBudget ) );

    std::vector<char> message( 1024 );
    for( size_t i = 0; i < message.size(); i++ )
        message[i] = (char)PatternByte( i );
    client.Send( message.data(), (unsigned int)message.size(), serverAddress, false );

    Packet* packet = 0;
    REQUIRE( ConnectionWaits::WaitUntil( [&] { return ( packet = server.Receive() ) != 0; }, LoopbackTCP::kWaitBudget ) );
    CHECK( packet->length == message.size() );
    CHECK( memcmp( packet->data, message.data(), message.size() ) == 0 );
    server.DeallocatePacket( packet );

    CHECK( server.GetMessageLengthCapCloseCount() == 0 );
    CHECK( server.HasLostConnection() == UNASSIGNED_SYSTEM_ADDRESS );

    client.Stop();
    server.Stop();
}

TEST_CASE( "TCPInterface stops reading a client at the incoming cap, and drops nothing", "[tcpinterface][network]" )
{
    WinsockScope winsock;

    constexpr unsigned int kIncomingCap = 256 * 1024;

    // The stream's upper bound: far more than the cap plus every kernel buffer between the
    // two ends, so an uncapped server - which reads everything the moment it arrives -
    // never stalls the sender and the whole stream goes through. Once the sender stalls,
    // the stream ends at the stall plus kResumeMargin, so draining reads only the cap, the
    // kernel buffers and the margin. The sender stalls under 1 MiB on Windows loopback;
    // draining, the capped server reads about one cap per receive-thread pass.
    constexpr size_t kStreamLength = 16 * 1024 * 1024;

    // Sent after the stall, so the drain proves flow resumes and those bytes come through
    // intact too.
    constexpr size_t kResumeMargin = 256 * 1024;

    // A send blocked when the stream's end moves was sized against the old end, so it has
    // to fit in the margin. Static so the sender's lambda can name it without capturing it.
    static constexpr size_t kChunkLength = 4096;
    static_assert( kChunkLength <= kResumeMargin, "a blocked send must fit in the margin" );

    TCPInterface server;
    REQUIRE( server.Start( kBackpressureListenPort, 4 ) );
    server.SetMaxIncomingBytesPerClient( kIncomingCap );
    CHECK( server.GetMaxIncomingBytesPerClient() == kIncomingCap );

    LoopbackTCP::Client client( kBackpressureListenPort, LoopbackTCP::kAnyPort, kSmallClientBuffers );
    REQUIRE( ConnectionWaits::WaitUntil( [&] { return server.HasNewIncomingConnection() != UNASSIGNED_SYSTEM_ADDRESS; }, LoopbackTCP::kWaitBudget ) );

    // Blocking sends on their own thread: a send that stops returning is the stall.
    std::atomic<size_t> sentLength( 0 );
    std::atomic<size_t> streamEnd( kStreamLength );
    const auto sendStream = [&sentLength, &streamEnd, clientSocket = client.Socket()]() {
        std::vector<char> chunk( kChunkLength );
        for( ;; )
        {
            const size_t offset = sentLength;
            const size_t end = streamEnd;
            if( offset >= end )
                return;
            const size_t length = ( std::min )( chunk.size(), end - offset );
            for( size_t i = 0; i < length; i++ )
                chunk[i] = (char)PatternByte( offset + i );
            const int sent = send__( clientSocket, chunk.data(), (int)length, 0 );
            if( sent <= 0 )
                return;
            sentLength += (size_t)sent;
        }
    };
    std::thread sender( sendStream );

    // Closing the socket is what unblocks a stalled send, so it comes before the join.
    struct SenderScope
    {
        std::thread& sender;
        LoopbackTCP::Client& client;
        ~SenderScope()
        {
            client.Abort();
            sender.join();
        }
    } senderScope{ sender, client };

    // The application does not poll. The sender has to stall well short of the stream.
    // Stalled means no progress for kStallWindow; every byte sent moves the deadline on.
    constexpr TimeMS kStallWindow = 500;
    size_t lastSentLength = 0;
    TimeMS stallDeadline = GetTimeMS() + kStallWindow;
    while( !ConnectionWaits::Expired( stallDeadline ) && sentLength < kStreamLength )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
        if( sentLength != lastSentLength )
        {
            lastSentLength = sentLength;
            stallDeadline = GetTimeMS() + kStallWindow;
        }
    }
    REQUIRE( sentLength < kStreamLength );
    CHECK( server.GetIncomingBytesCapStallCount() > 0 );

    // The sender is stalled, so it is still short of the new end.
    streamEnd = sentLength + kResumeMargin;

    // Draining lets the stream through, every byte in order.
    size_t receivedLength = 0;
    bool isIntact = true;
    const TimeMS deadline = GetTimeMS() + 30000;
    while( receivedLength < streamEnd && !ConnectionWaits::Expired( deadline ) )
    {
        Packet* packet = server.Receive();
        if( packet == 0 )
        {
            std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
            continue;
        }
        for( unsigned int i = 0; i < packet->length && isIntact; i++ )
            isIntact = packet->data[i] == PatternByte( receivedLength + i );
        receivedLength += packet->length;
        server.DeallocatePacket( packet );
    }
    CHECK( isIntact );
    CHECK( receivedLength == streamEnd );

    server.Stop();
}

TEST_CASE( "TCPInterface closes a client that never reads at the outgoing cap", "[tcpinterface][network]" )
{
    WinsockScope winsock;

    constexpr unsigned int kOutgoingCap = 64 * 1024;

    TCPInterface server;
    REQUIRE( server.Start( kNeverReadsListenPort, 4 ) );
    server.SetMaxOutgoingBytesPerClient( kOutgoingCap );
    CHECK( server.GetMaxOutgoingBytesPerClient() == kOutgoingCap );

    LoopbackTCP::Client client( kNeverReadsListenPort, LoopbackTCP::kAnyPort, kSmallClientBuffers );
    SystemAddress clientAddress = UNASSIGNED_SYSTEM_ADDRESS;
    REQUIRE( ConnectionWaits::WaitUntil( [&] { return ( clientAddress = server.HasNewIncomingConnection() ) != UNASSIGNED_SYSTEM_ADDRESS; }, LoopbackTCP::kWaitBudget ) );

    // The client never reads, so once the kernel buffers are full everything sent waits in
    // outgoingData. Bounded, so an uncapped server fails here rather than exhausting memory.
    // Several chunks a poll, so the wait sends far more than any autotuned send buffer holds:
    // eight 16 KiB chunks every 30 ms is about 20 MiB inside the wait.
    constexpr size_t kSendBound = 256u * 1024 * 1024;
    constexpr int kChunksPerPoll = 8;
    const std::vector<char> chunk( 16 * 1024, 'x' );
    SystemAddress lostAddress = UNASSIGNED_SYSTEM_ADDRESS;
    size_t sentLength = 0;
    unsigned int largestBuffered = 0;
    CHECK( ConnectionWaits::WaitUntil(
        [&] {
            for( int i = 0; i < kChunksPerPoll && sentLength < kSendBound; i++ )
            {
                server.Send( chunk.data(), (unsigned int)chunk.size(), clientAddress, false );
                sentLength += chunk.size();
                largestBuffered = (std::max)( largestBuffered, server.GetOutgoingDataBufferSize( clientAddress ) );
            }
            return ( lostAddress = server.HasLostConnection() ) != UNASSIGNED_SYSTEM_ADDRESS;
        },
        LoopbackTCP::kWaitBudget ) );
    CHECK( largestBuffered <= kOutgoingCap );
    CHECK( lostAddress == clientAddress );
    CHECK( server.GetOutgoingBytesCapCloseCount() == 1 );
    CHECK( server.GetConnectionCount() == 0 );

    client.Abort();
    server.Stop();
}

TEST_CASE( "PacketizedTCP keeps a client reconnecting from the same address before its lost event", "[packetizedtcp][network]" )
{
    WinsockScope winsock;

    InspectablePacketizedTCP server;
    REQUIRE( server.Start( kReconnectListenPort, 4 ) );

    LoopbackTCP::Client first( kReconnectListenPort, kReconnectClientPort );
    SystemAddress firstAddress = UNASSIGNED_SYSTEM_ADDRESS;
    REQUIRE( ConnectionWaits::WaitUntil( [&] { return ( firstAddress = server.HasNewIncomingConnection() ) != UNASSIGNED_SYSTEM_ADDRESS; }, LoopbackTCP::kWaitBudget ) );
    REQUIRE( server.ConnectionEntryCount() == 1 );

    // The first connection goes, and TCPInterface has queued its lost event - its slot is
    // free - before the reconnect arrives. The application has not polled since.
    first.Abort();
    REQUIRE( ConnectionWaits::WaitUntil( [&] { return server.GetConnectionCount() == 0; }, LoopbackTCP::kWaitBudget ) );

    LoopbackTCP::Client second( kReconnectListenPort, kReconnectClientPort );
    REQUIRE( ConnectionWaits::WaitUntil( [&] { return server.GetConnectionCount() == 1; }, LoopbackTCP::kWaitBudget ) );

    const std::vector<char> message( 100, 'y' );
    const std::vector<char> framed = Frame( message );
    second.SendAll( framed.data(), framed.size() );
    REQUIRE( ConnectionWaits::WaitUntil( [&] { return server.ReceiveHasPackets(); }, LoopbackTCP::kWaitBudget ) );

    // Both events and the message are queued now: the new event is processed first, then
    // the lost one, then the message. The entry has to survive the lost event.
    Packet* packet = 0;
    CHECK( ConnectionWaits::WaitUntil( [&] { return ( packet = server.Receive() ) != 0; }, LoopbackTCP::kWaitBudget ) );
    if( packet != 0 )
    {
        CHECK( packet->systemAddress == firstAddress );
        CHECK( packet->length == message.size() );
        server.DeallocatePacket( packet );
    }
    CHECK( server.ConnectionEntryCount() == 1 );

    // Each connection is still reported once each way.
    CHECK( server.HasNewIncomingConnection() == firstAddress );
    CHECK( server.HasLostConnection() == firstAddress );

    // And the reconnect's own loss frees the entry.
    second.Abort();
    CHECK( ConnectionWaits::WaitUntil( [&] { return server.HasLostConnection() == firstAddress; }, LoopbackTCP::kWaitBudget ) );
    CHECK( server.ConnectionEntryCount() == 0 );

    server.Stop();
}
