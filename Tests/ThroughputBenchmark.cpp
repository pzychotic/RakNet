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

#if defined( _WIN32 )
#include "WindowsIncludes.h"
#else
#include <time.h>
#endif

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
without it: Catch2's default is 100 samples, and one sample here takes seconds. Catch2
prints its own mean and standard deviation of the wall time. Each case also prints the
medians ADR-0009 judges by, of the samples' wall time and of the CPU work the whole
process did during them, in megacycles on Windows and CPU milliseconds elsewhere:

    ThroughputBenchmark: 32 B: median 800.4 ms wall, 1493 Mcycles of 5 runs

Judge by the CPU work. With 32 B messages the wall time is held by how many reliable
messages may await an ack (RESEND_BUFFER_ARRAY_LENGTH) per round trip, about two
milliseconds on loopback with the receiver's ack gap, so a slower allocator barely moves
it. The CPU work is what both Peers did to move the messages, which is what an
allocator changes; on an idle machine its median repeats within 2% for every case. The
wait for the last message sleeps rather than spins, so it adds little work of its own.

To compare two builds, run the command above on each, on an idle machine, one after the
other, and compare the medians case by case. ADR-0009's threshold is 5% on every case.
Interactive use of the machine during a run moved the 1 KB wall time by over 30%. Run
each build several times, alternating, and judge a difference only against the spread
between runs of the same build.

A run that does not deliver every message inside kRunBudgetMs fails the test case,
naming how far it got and what the sender still held.
*/

using namespace RakNet;

namespace {

constexpr int kMessageCount = 200000;

constexpr MessageID kMessageId = ID_USER_PACKET_ENUM + 1;

constexpr char kChannel = 0;

// Hang guard for one run, not a timeout to tune: the slowest case takes about 3 s.
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
        // Receive does not wake either Peer's update thread, so sleeping here leaves
        // the transfer's pace alone and keeps this thread's CPU time out of the run's.
        if( received == before )
        {
            std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
        }
    }
    return {};
}

/// The CPU work the whole process has done so far, every thread of both Peers and this
/// one, in user and kernel mode, in kCpuWorkUnit.
///
/// On Windows it is counted in cycles: GetProcessTimes charges whole 15.6 ms timer ticks
/// to whichever thread a tick lands on, too coarse for a run that uses under a second of
/// CPU. Elsewhere the process CPU clock is exact.
#if defined( _WIN32 )
constexpr const char* kCpuWorkUnit = "Mcycles";

double ProcessCpuWork()
{
    ULONG64 cycles = 0;
    QueryProcessCycleTime( GetCurrentProcess(), &cycles );
    return (double)cycles / 1e6;
}
#else
constexpr const char* kCpuWorkUnit = "ms CPU";

double ProcessCpuWork()
{
    timespec now{};
    clock_gettime( CLOCK_PROCESS_CPUTIME_ID, &now );
    return (double)now.tv_sec * 1000.0 + (double)now.tv_nsec / 1000000.0;
}
#endif

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
    std::vector<double> runCpuWork;

    BENCHMARK_ADVANCED( benchCase.name )( Catch::Benchmark::Chronometer meter )
    {
        meter.measure( [&] {
            const auto start = std::chrono::steady_clock::now();
            const double startCpuWork = ProcessCpuWork();
            const std::string failure = SendAndReceiveAll( sender, receiver, message );
            runCpuWork.push_back( ProcessCpuWork() - startCpuWork );
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
    const std::vector<double> sampleCpuWork( runCpuWork.end() - kSamples, runCpuWork.end() );
    std::printf( "\nThroughputBenchmark: %s: median %.1f ms wall, %.0f %s of %u runs\n", benchCase.name, Median( sampleMs ),
                 Median( sampleCpuWork ), kCpuWorkUnit, kSamples );
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
