#include "ConnectionWaits.h"
#include "PeerScope.h"

#include "GetTime.h"
#include "MTUSize.h"
#include "MessageIdentifiers.h"
#include "PacketPriority.h"
#include "RakNetStatistics.h"
#include "RakNetTime.h"
#include "RakPeerInterface.h"

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/interfaces/catch_interfaces_config.hpp>
#include <catch2/internal/catch_context.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

/*
How fast two Peers move RELIABLE_ORDERED messages over loopback (ADR-0009).

One Peer sends 200 000 messages on one channel as fast as Send takes them, and a run
ends when the other has Received the last of them. Three cases: 32 B, 1 KB, and 4 KB,
which is over the MTU so every message is split. Each case gets a connection of its
own, made outside the timing.

Hidden under [.bench], so ctest and CI never run it. Build Release and run it on its own:

    cmake --build build --config Release --target RakNetTests
    build/Tests/Release/RakNetTests.exe -# "[#ThroughputBenchmark]" "[.bench]" --benchmark-samples 5

--benchmark-samples 5 is part of the command, not a tuning knob, and the test case fails
without it: Catch2's default is 100 samples, and one sample here takes seconds. Catch2 prints its own mean and standard
deviation. Each case also prints the number ADR-0009 judges by, the median of the
samples:

    ThroughputBenchmark: 32 B: median 6533.4 ms of 5 runs

To compare two builds, run the command above on each, on an idle machine, one after the
other, and compare the medians case by case. ADR-0009's threshold is 5% on every case,
and the 1 KB case alone can move more than that between two runs of the same build, so
run each build at least twice, alternating, and compare against that spread.

A run that does not deliver every message inside kRunBudgetMs fails the test case,
naming how far it got and what the sender still held.
*/

using namespace RakNet;

namespace {

constexpr int kMessageCount = 200000;

constexpr MessageID kMessageId = ID_USER_PACKET_ENUM + 1;

constexpr char kChannel = 0;

// Hang guard for one run, not a timeout to tune: the slowest case takes about 16 s.
constexpr TimeMS kRunBudgetMs = 60000;

// How many messages go out between two drains of the receiver while sending.
constexpr int kSendsPerDrain = 1024;

// Over the MTU whatever the connection negotiates, so every message is split.
constexpr int kSplitMessageBytes = 4096;
static_assert( kSplitMessageBytes > MAXIMUM_MTU_SIZE, "the split case must be over the MTU" );

// ADR-0009 judges by the median of this many runs.
constexpr unsigned int kSamples = 5;

struct BenchCase
{
    const char* name;
    int bytes;
};

/// Receives and deallocates everything queued on \a receiver, adding the benchmark's
/// messages to \a received. Returns false if the connection was lost.
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

/// Why a run stopped short: how far it got, and what each end still held.
std::string DescribeFailure( const char* why, RakPeerInterface* sender, RakPeerInterface* receiver, int sent, int received )
{
    std::ostringstream text;
    text << why << " after " << sent << " sent and " << received << " received";

    RakNetStatistics sending{};
    if( sender->GetStatistics( sender->GetSystemAddressFromGuid( receiver->GetMyGUID() ), &sending ) != nullptr )
    {
        unsigned int queued = 0;
        for( unsigned int messages : sending.messageInSendBuffer )
        {
            queued += messages;
        }
        text << "; sender has " << queued << " messages queued and " << sending.messagesInResendBuffer << " awaiting an ack";
    }

    RakNetStatistics receiving{};
    if( receiver->GetStatistics( receiver->GetSystemAddressFromGuid( sender->GetMyGUID() ), &receiving ) != nullptr )
    {
        text << "; receiver holds " << receiving.bytesHeldForReassemblyAndOrdering << " bytes for reassembly and ordering";
    }
    return text.str();
}

/// One run: sends kMessageCount copies of \a message from \a sender to \a receiver and
/// returns once the receiver has Received every one. Returns why it stopped short, or an
/// empty string when every message arrived.
std::string SendAndReceiveAll( RakPeerInterface* sender, RakPeerInterface* receiver, const std::vector<char>& message )
{
    const RakNetGUID target = receiver->GetMyGUID();
    const TimeMS deadline = GetTimeMS() + kRunBudgetMs;
    int received = 0;

    for( int sent = 0; sent < kMessageCount; ++sent )
    {
        if( sender->Send( message.data(), (int)message.size(), HIGH_PRIORITY, RELIABLE_ORDERED, kChannel, target, false ) == 0 )
        {
            return DescribeFailure( "Send refused a message", sender, receiver, sent, received );
        }
        if( sent % kSendsPerDrain == 0 && !DrainCounting( receiver, received ) )
        {
            return DescribeFailure( "the connection was lost", sender, receiver, sent, received );
        }
    }

    while( received < kMessageCount )
    {
        const int before = received;
        if( !DrainCounting( receiver, received ) )
        {
            return DescribeFailure( "the connection was lost", sender, receiver, kMessageCount, received );
        }
        if( ConnectionWaits::Expired( deadline ) )
        {
            return DescribeFailure( "the run budget ran out", sender, receiver, kMessageCount, received );
        }
        if( received == before )
        {
            std::this_thread::yield();
        }
    }
    return {};
}

double Median( std::vector<double> values )
{
    std::sort( values.begin(), values.end() );
    const size_t middle = values.size() / 2;
    return values.size() % 2 == 1 ? values[middle] : ( values[middle - 1] + values[middle] ) / 2;
}

/// Connects a fresh pair of Peers and benchmarks one case on it, then prints the median
/// of the samples.
void RunCase( const BenchCase& benchCase )
{
    PeerScope peers;
    RakPeerInterface* receiver = peers.Server( 0 );
    RakPeerInterface* sender = peers.Client();
    ConnectionWaits::ConnectAndWait( sender, receiver );

    std::vector<char> message( (size_t)benchCase.bytes, 'x' );
    message[0] = (char)kMessageId;

    // Catch2 runs the measured code once more than it takes samples, to estimate its
    // length, so the samples are the last kSamples runs recorded here.
    std::vector<double> runMs;

    BENCHMARK_ADVANCED( benchCase.name )( Catch::Benchmark::Chronometer meter )
    {
        meter.measure( [&] {
            const auto start = std::chrono::steady_clock::now();
            const std::string failure = SendAndReceiveAll( sender, receiver, message );
            runMs.push_back( std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - start ).count() );
            if( !failure.empty() )
            {
                FAIL( benchCase.name << ": " << failure );
            }
        } );
    };

    // Nothing ran under --skip-benchmarks.
    if( runMs.size() < kSamples )
    {
        return;
    }
    const std::vector<double> sampleMs( runMs.end() - kSamples, runMs.end() );
    std::printf( "\nThroughputBenchmark: %s: median %.1f ms of %u runs\n", benchCase.name, Median( sampleMs ), kSamples );
    std::fflush( stdout );
}

} // namespace

TEST_CASE( "ThroughputBenchmark: 200 000 RELIABLE_ORDERED messages over loopback", "[.bench]" )
{
    // Catch2's default of 100 samples would take hours.
    INFO( "run with --benchmark-samples " << kSamples );
    REQUIRE( Catch::getCurrentContext().getConfig()->benchmarkSamples() == kSamples );

    RunCase( { "32 B", 32 } );
    RunCase( { "1 KB", 1024 } );
    RunCase( { "4 KB, split", kSplitMessageBytes } );
}
