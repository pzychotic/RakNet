#include "ConnectionWaits.h"
#include "PeerScope.h"

#include "GetTime.h"
#include "MessageIdentifiers.h"
#include "PacketPriority.h"
#include "RakNetStatistics.h"
#include "RakNetTime.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <sstream>
#include <string>
#include <thread>
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
30 ms on loopback does not survive.
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
    PeerScope peers;
    RakPeerInterface* receiver = peers.Server( 0 );
    RakPeerInterface* sender = peers.Client();
    ConnectionWaits::ConnectAndWait( sender, receiver );

    REQUIRE( Transfer( sender, receiver, kWarmUpMessages, kWarmUpBytes ) );
    Snapshot before;
    REQUIRE( SettleAndRead( sender, receiver, before ) );
    const uint64_t senderCapDropsBefore = sender->GetReceivedDatagramsDroppedAtCap();
    const uint64_t receiverCapDropsBefore = receiver->GetReceivedDatagramsDroppedAtCap();

    StallMonitor stalls;
    const TimeUS burstStart = GetTimeUS();
    TimeUS sendsQueuedAt = 0;
    REQUIRE( Transfer( sender, receiver, kBurstMessages, kBurstBytes, &sendsQueuedAt ) );
    const TimeUS burstEnd = GetTimeUS();
    stalls.Stop();
    Snapshot after;
    REQUIRE( SettleAndRead( sender, receiver, after ) );

    std::ostringstream report;
    report << "burst: sends queued at " << ( sendsQueuedAt - burstStart ) / 1000 << " ms, all received at " << ( burstEnd - burstStart ) / 1000 << " ms"
           << "\nsender resent " << Delta( before.sender, after.sender, USER_MESSAGE_BYTES_RESENT ) << " B of " << Delta( before.sender, after.sender, USER_MESSAGE_BYTES_SENT ) << " B sent"
           << "\nlost sender->receiver " << Lost( before.sender, after.sender, before.receiver, after.receiver ) << " B, receiver->sender (acks) "
           << Lost( before.receiver, after.receiver, before.sender, after.sender ) << " B"
           << "\ndropped at receive cap: sender " << sender->GetReceivedDatagramsDroppedAtCap() - senderCapDropsBefore << ", receiver "
           << receiver->GetReceivedDatagramsDroppedAtCap() - receiverCapDropsBefore << "\nstall monitor: " << stalls.Report();
    INFO( report.str() );

    CHECK( Delta( before.receiver, after.receiver, USER_MESSAGE_BYTES_RECEIVED_IGNORED ) == 0 );
}
