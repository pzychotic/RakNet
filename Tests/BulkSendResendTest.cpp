#include "ConnectionWaits.h"
#include "PeerScope.h"

#include "GetTime.h"
#include "InternalPacket.h"
#include "MessageIdentifiers.h"
#include "PacketPriority.h"
#include "PluginInterface2.h"
#include "RakNetStatistics.h"
#include "RakNetTime.h"
#include "RakNetTypes.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

/*
Pins that a burst of Sends on a warm connection is never resent on a timeout when nothing
was lost.

One update cycle applies every Send queued since the last, and a burst queued faster than
the network thread applies it keeps that cycle applying it for tens of milliseconds. The
datagrams the cycle then sends are timed from when they leave, not from when the cycle
began, so none times out before its ack can arrive.

The connection first carries a warm-up transfer, so the send window is open at its cap and
the burst's first window goes out at once. A datagram the kernel drops is resent but never
arrives twice, so any byte the receiver ignores as a duplicate was resent although its
original arrived.

On a failure the test reports what the burst saw: bytes each side sent that the other never
received (a lost ack also gets its datagrams resent), datagrams dropped at the receive cap,
and every gap of kStallReportUs or more in a thread that wakes once a millisecond. A gap
there means the whole process went unscheduled, which a retransmission timeout of about
30 ms on loopback does not survive. A plugin on each peer logs every message's send,
resend, receive and ack, so the report also says where the first resent messages were
held up and which clock the sender's reliability layer used.
*/

using namespace RakNet;

namespace {

constexpr MessageID kMessageId = ID_USER_PACKET_ENUM + 1;

constexpr int kWarmUpMessages = 5000;
constexpr int kWarmUpBytes = 1024;

// Enough small messages that applying them all outlasts the retransmission timeout.
constexpr int kBurstMessages = 300000;
constexpr int kBurstBytes = 32;

// How many messages go out between two drains of the receiver while sending.
constexpr int kSendsPerDrain = 1024;

// Hang guard for one transfer, not a timeout to tune.
constexpr TimeMS kTransferBudgetMs = 60000;

// A gap the stall monitor reports. Windows rounds its 1 ms sleep up to the ~15.6 ms timer
// tick, so a gap below this is the sleep, not a stall.
constexpr TimeUS kStallReportUs = 25000;
constexpr size_t kMaxStallsReported = 16;

// How long the connection stays quiet after its last ack before statistics are read, so
// the pings and acks still in flight have landed and the two sides' byte counts compare.
constexpr std::chrono::milliseconds kSettle( 100 );

/// Records how long a thread that sleeps 1 ms at a time goes without running.
class StallMonitor
{
public:
    struct Stall
    {
        TimeUS startUs; // from the monitor's start
        TimeUS lengthUs;
    };

    StallMonitor()
    : start( GetTimeUS() )
    , thread( [this] { Run(); } )
    {
    }

    ~StallMonitor() { Stop(); }

    void Stop()
    {
        stop = true;
        if( thread.joinable() )
        {
            thread.join();
        }
    }

    /// Call after Stop.
    std::string Report() const
    {
        std::ostringstream out;
        out << "longest gap " << maxGapUs / 1000 << " ms, " << stallCount << " gaps >= " << kStallReportUs / 1000 << " ms";
        for( const Stall& stall : stalls )
        {
            out << "\n  at " << stall.startUs / 1000 << " ms for " << stall.lengthUs / 1000 << " ms";
        }
        return out.str();
    }

private:
    void Run()
    {
        TimeUS last = GetTimeUS();
        while( !stop )
        {
            std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
            const TimeUS now = GetTimeUS();
            const TimeUS gap = now - last;
            maxGapUs = (std::max)( maxGapUs, gap );
            if( gap >= kStallReportUs )
            {
                ++stallCount;
                if( stalls.size() < kMaxStallsReported )
                {
                    stalls.push_back( { last - start, gap } );
                }
            }
            last = now;
        }
    }

    const TimeUS start;
    std::atomic<bool> stop{ false };
    TimeUS maxGapUs = 0;
    size_t stallCount = 0;
    std::vector<Stall> stalls;
    std::thread thread;
};

/// Statistics of both ends of the connection, read together.
struct Snapshot
{
    RakNetStatistics sender{};
    RakNetStatistics receiver{};
};

/// Waits until \a sender has every reliable message acked and the connection has been
/// quiet for kSettle, then reads both ends' statistics. Returns false if either end has no
/// connection or the acks never come.
bool SettleAndRead( RakPeerInterface* sender, RakPeerInterface* receiver, Snapshot& snapshot )
{
    const SystemAddress toReceiver = sender->GetSystemAddressFromGuid( receiver->GetMyGUID() );
    const SystemAddress toSender = receiver->GetSystemAddressFromGuid( sender->GetMyGUID() );
    const TimeMS deadline = GetTimeMS() + kTransferBudgetMs;
    while( true )
    {
        if( sender->GetStatistics( toReceiver, &snapshot.sender ) == nullptr )
        {
            return false;
        }
        if( snapshot.sender.messagesInResendBuffer == 0 )
        {
            break;
        }
        if( ConnectionWaits::Expired( deadline ) )
        {
            return false;
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }
    std::this_thread::sleep_for( kSettle );
    return sender->GetStatistics( toReceiver, &snapshot.sender ) != nullptr && receiver->GetStatistics( toSender, &snapshot.receiver ) != nullptr;
}

uint64_t Delta( const RakNetStatistics& before, const RakNetStatistics& after, RNSPerSecondMetrics metric )
{
    return after.runningTotal[metric] - before.runningTotal[metric];
}

/// Bytes one end sent over the burst that the other end never took in.
long long Lost( const RakNetStatistics& fromBefore, const RakNetStatistics& fromAfter, const RakNetStatistics& toBefore, const RakNetStatistics& toAfter )
{
    return (long long)Delta( fromBefore, fromAfter, ACTUAL_BYTES_SENT ) - (long long)Delta( toBefore, toAfter, ACTUAL_BYTES_RECEIVED );
}

// Events one Timeline holds: a burst's sends and acks on the sender, or its receives with
// their duplicates on the receiver, with room for a few hundred thousand resends.
constexpr size_t kTimelineCapacity = 800000;
constexpr size_t kResendsExplained = 8;

/// Logs, on the network thread, when the reliability layer sends, resends, receives and
/// gets an ack for each of the test's reliable messages. A plugin that uses the
/// reliability layer is attached before Startup.
class Timeline : public PluginInterface2
{
public:
    enum Kind : uint8_t
    {
        kSend, // the first is the original, any later one a resend
        kReceive,
        kAck,
    };

    struct Event
    {
        TimeUS realUs;  // GetTimeUS() in the callback
        TimeMS layerMs; // the time the reliability layer passed: its cycle's clock, or when the datagram was read
        TimeUS rtoUs;   // on a send, the retransmission timeout it was given
        uint32_t number;
        Kind kind;
    };

    Timeline()
    : events( kTimelineCapacity )
    {
    }

    void Enable( bool on ) { enabled.store( on, std::memory_order_relaxed ); }

    /// Every event logged so far, oldest first.
    std::vector<Event> Events() const
    {
        const size_t n = count.load( std::memory_order_acquire );
        return std::vector<Event>( events.begin(), events.begin() + (std::ptrdiff_t)n );
    }

    size_t Overflowed() const { return overflowed.load( std::memory_order_relaxed ); }

    bool UsesReliabilityLayer( void ) const override { return true; }

    void OnInternalPacket( InternalPacket* internalPacket, unsigned frameNumber, SystemAddress remoteSystemAddress, TimeMS time, int isSend ) override
    {
        (void)frameNumber;
        (void)remoteSystemAddress;
        if( internalPacket->dataBitLength < 8 || internalPacket->data[0] != kMessageId )
        {
            return;
        }
        Push( { GetTimeUS(), time, isSend ? internalPacket->retransmissionTime : 0, internalPacket->reliableMessageNumber.val, isSend ? kSend : kReceive } );
    }

    void OnAck( unsigned int messageNumber, SystemAddress remoteSystemAddress, TimeMS time ) override
    {
        (void)remoteSystemAddress;
        Push( { GetTimeUS(), time, 0, messageNumber, kAck } );
    }

private:
    void Push( const Event& event )
    {
        if( !enabled.load( std::memory_order_relaxed ) )
        {
            return;
        }
        const size_t n = count.load( std::memory_order_relaxed );
        if( n == events.size() )
        {
            overflowed.fetch_add( 1, std::memory_order_relaxed );
            return;
        }
        events[n] = event;
        count.store( n + 1, std::memory_order_release );
    }

    std::vector<Event> events;
    std::atomic<size_t> count{ 0 };
    std::atomic<size_t> overflowed{ 0 };
    std::atomic<bool> enabled{ false };
};

/// One message's way through the burst, in GetTimeUS() time. 0 where it never happened.
struct MessagePath
{
    TimeUS sentUs = 0;
    TimeMS sentLayerMs = 0;
    TimeUS rtoUs = 0;
    TimeUS handledUs = 0; // the receiver's network thread took in the first copy
    TimeMS readMs = 0;    // the receiver's receive thread read the first copy
    TimeUS ackedUs = 0;
    std::vector<Timeline::Event> resends;
};

/// Says where the burst's resent messages were held up, from both ends' timelines.
std::string ExplainResends( const Timeline& sender, const Timeline& receiver, TimeUS burstStartUs )
{
    std::unordered_map<uint32_t, MessagePath> paths;
    for( const Timeline::Event& event : sender.Events() )
    {
        MessagePath& path = paths[event.number];
        if( event.kind == Timeline::kAck && path.ackedUs == 0 )
        {
            path.ackedUs = event.realUs;
        }
        else if( event.kind == Timeline::kSend && path.sentUs == 0 )
        {
            path.sentUs = event.realUs;
            path.sentLayerMs = event.layerMs;
            path.rtoUs = event.rtoUs;
        }
        else if( event.kind == Timeline::kSend )
        {
            path.resends.push_back( event );
        }
    }
    for( const Timeline::Event& event : receiver.Events() )
    {
        auto it = paths.find( event.number );
        if( event.kind == Timeline::kReceive && it != paths.end() && it->second.handledUs == 0 )
        {
            it->second.handledUs = event.realUs;
            it->second.readMs = event.layerMs;
        }
    }

    const auto fixed1 = []( double value ) {
        std::ostringstream out;
        out.setf( std::ios::fixed );
        out.precision( 1 );
        out << value;
        return out.str();
    };
    const auto ms = [&]( TimeUS us ) { return fixed1( ( (double)us - (double)burstStartUs ) / 1000.0 ); };
    const auto span = [&]( TimeUS us ) { return fixed1( (double)us / 1000.0 ) + " ms"; };
    const auto layerMs = [burstStartUs]( TimeMS t ) {
        std::ostringstream out;
        out.setf( std::ios::fixed );
        out.precision( 0 );
        out << (double)t - (double)burstStartUs / 1000.0;
        return out.str();
    };

    // Over every message that went the whole way: how long each leg took at most.
    TimeUS maxSendLagUs = 0, maxHandleLagUs = 0, maxAckLagUs = 0;
    uint32_t maxSendLagAt = 0, maxHandleLagAt = 0, maxAckLagAt = 0;
    size_t resentMessages = 0, resendEvents = 0;
    std::vector<std::pair<TimeUS, uint32_t>> firstResends;
    for( const auto& [number, path] : paths )
    {
        if( path.sentUs == 0 )
        {
            continue;
        }
        const TimeUS sentLayerUs = (TimeUS)path.sentLayerMs * 1000;
        if( path.sentUs > sentLayerUs && path.sentUs - sentLayerUs > maxSendLagUs )
        {
            maxSendLagUs = path.sentUs - sentLayerUs;
            maxSendLagAt = number;
        }
        if( path.handledUs > path.sentUs && path.handledUs - path.sentUs > maxHandleLagUs )
        {
            maxHandleLagUs = path.handledUs - path.sentUs;
            maxHandleLagAt = number;
        }
        if( path.handledUs != 0 && path.ackedUs > path.handledUs && path.ackedUs - path.handledUs > maxAckLagUs )
        {
            maxAckLagUs = path.ackedUs - path.handledUs;
            maxAckLagAt = number;
        }
        if( !path.resends.empty() )
        {
            ++resentMessages;
            resendEvents += path.resends.size();
            firstResends.emplace_back( path.resends.front().realUs, number );
        }
    }
    std::sort( firstResends.begin(), firstResends.end() );

    std::ostringstream out;
    out << "timeline (ms from burst start; [n] = the reliability layer's clock):\n  " << resentMessages << " messages resent " << resendEvents << " times; timeline overflow: sender "
        << sender.Overflowed() << ", receiver " << receiver.Overflowed();
    const auto leg = [&]( const char* what, TimeUS lagUs, uint32_t number ) {
        out << "\n  longest " << what << ": " << span( lagUs ) << ", message " << number;
        const MessagePath& path = paths[number];
        out << " sent " << ms( path.sentUs ) << " handled " << ms( path.handledUs ) << " acked " << ms( path.ackedUs );
    };
    leg( "send behind its cycle's clock", maxSendLagUs, maxSendLagAt );
    leg( "send to receiver handling it", maxHandleLagUs, maxHandleLagAt );
    leg( "receiver handling to sender's ack", maxAckLagUs, maxAckLagAt );
    // One line per resending cycle, for the first message it resent.
    size_t listed = 0;
    for( size_t i = 0; i < firstResends.size() && listed < kResendsExplained; )
    {
        size_t end = i + 1;
        while( end < firstResends.size() && firstResends[end].first - firstResends[i].first < 1000 )
        {
            ++end;
        }
        const uint32_t number = firstResends[i].second;
        const MessagePath& path = paths[number];
        const Timeline::Event& resend = path.resends.front();
        out << "\n  message " << number << ": sent " << ms( path.sentUs ) << " [" << layerMs( path.sentLayerMs ) << "] rto " << span( path.rtoUs ) << ", read "
            << layerMs( path.readMs ) << ", handled " << ( path.handledUs ? ms( path.handledUs ) : std::string( "never" ) ) << ", resent " << ms( resend.realUs ) << " ["
            << layerMs( resend.layerMs ) << "], acked " << ( path.ackedUs ? ms( path.ackedUs ) : std::string( "never" ) ) << "; " << end - i
            << " messages resent within 1 ms";
        ++listed;
        i = end;
    }
    return out.str();
}

/// Receives and deallocates everything queued on \a receiver, adding the test's messages
/// to \a received. Returns false if the connection was lost.
bool DrainCounting( RakPeerInterface* receiver, int& received )
{
    for( Packet* packet = receiver->Receive(); packet != nullptr; packet = receiver->Receive() )
    {
        const MessageID id = packet->data[0];
        receiver->DeallocatePacket( packet );
        if( id == kMessageId )
        {
            ++received;
        }
        else if( id == ID_CONNECTION_LOST || id == ID_DISCONNECTION_NOTIFICATION )
        {
            return false;
        }
    }
    return true;
}

/// Sends \a count reliable messages of \a bytes each as fast as Send takes them, and
/// returns once the receiver has Received every one, or false if it never does. Stores the
/// time the last Send returned in \a sendsQueuedAt if it is given.
bool Transfer( RakPeerInterface* sender, RakPeerInterface* receiver, int count, int bytes, TimeUS* sendsQueuedAt = nullptr )
{
    std::vector<char> message( (size_t)bytes, 'x' );
    message[0] = (char)kMessageId;
    const RakNetGUID target = receiver->GetMyGUID();
    const TimeMS deadline = GetTimeMS() + kTransferBudgetMs;
    int received = 0;

    for( int sent = 0; sent < count; ++sent )
    {
        if( sender->Send( message.data(), bytes, HIGH_PRIORITY, RELIABLE_ORDERED, 0, target, false ) == 0 )
        {
            return false;
        }
        if( sent % kSendsPerDrain == 0 && !DrainCounting( receiver, received ) )
        {
            return false;
        }
    }
    if( sendsQueuedAt != nullptr )
    {
        *sendsQueuedAt = GetTimeUS();
    }

    while( received < count )
    {
        const int before = received;
        if( !DrainCounting( receiver, received ) || ConnectionWaits::Expired( deadline ) )
        {
            return false;
        }
        if( received == before )
        {
            std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
        }
    }
    return true;
}

} // namespace

TEST_CASE( "A burst of Sends on a warm connection is not resent while its acks are on the way", "[network]" )
{
    // Before the peers, so they outlive the RakPeers they are attached to.
    Timeline senderTimeline;
    Timeline receiverTimeline;
    PeerScope peers;
    RakPeerInterface* receiver = peers.Create();
    RakPeerInterface* sender = peers.Create();
    receiver->AttachPlugin( &receiverTimeline );
    sender->AttachPlugin( &senderTimeline );
    SocketDescriptor socketDescriptor( 0, 0 );
    REQUIRE( receiver->Startup( 1, &socketDescriptor, 1 ) == RAKNET_STARTED );
    receiver->SetMaximumIncomingConnections( 1 );
    REQUIRE( sender->Startup( 1, &socketDescriptor, 1 ) == RAKNET_STARTED );
    ConnectionWaits::ConnectAndWait( sender, receiver );

    REQUIRE( Transfer( sender, receiver, kWarmUpMessages, kWarmUpBytes ) );
    Snapshot before;
    REQUIRE( SettleAndRead( sender, receiver, before ) );
    const uint64_t senderCapDropsBefore = sender->GetReceivedDatagramsDroppedAtCap();
    const uint64_t receiverCapDropsBefore = receiver->GetReceivedDatagramsDroppedAtCap();

    StallMonitor stalls;
    senderTimeline.Enable( true );
    receiverTimeline.Enable( true );
    const TimeUS burstStart = GetTimeUS();
    TimeUS sendsQueuedAt = 0;
    REQUIRE( Transfer( sender, receiver, kBurstMessages, kBurstBytes, &sendsQueuedAt ) );
    const TimeUS burstEnd = GetTimeUS();
    stalls.Stop();
    Snapshot after;
    REQUIRE( SettleAndRead( sender, receiver, after ) );
    senderTimeline.Enable( false );
    receiverTimeline.Enable( false );
    const uint64_t ignored = Delta( before.receiver, after.receiver, USER_MESSAGE_BYTES_RECEIVED_IGNORED );

    std::ostringstream report;
    report << "burst: sends queued at " << ( sendsQueuedAt - burstStart ) / 1000 << " ms, all received at " << ( burstEnd - burstStart ) / 1000 << " ms"
           << "\nsender resent " << Delta( before.sender, after.sender, USER_MESSAGE_BYTES_RESENT ) << " B of " << Delta( before.sender, after.sender, USER_MESSAGE_BYTES_SENT ) << " B sent"
           << "\nlost sender->receiver " << Lost( before.sender, after.sender, before.receiver, after.receiver ) << " B, receiver->sender (acks) "
           << Lost( before.receiver, after.receiver, before.sender, after.sender ) << " B"
           << "\ndropped at receive cap: sender " << sender->GetReceivedDatagramsDroppedAtCap() - senderCapDropsBefore << ", receiver "
           << receiver->GetReceivedDatagramsDroppedAtCap() - receiverCapDropsBefore << "\nstall monitor: " << stalls.Report();
    // Matching every message's events takes a while in Debug, so only a failure pays for it.
    if( ignored != 0 )
    {
        report << "\n"
               << ExplainResends( senderTimeline, receiverTimeline, burstStart );
    }
    INFO( report.str() );

    CHECK( ignored == 0 );
}
