#include "Plugins/PacketFileLogger.h"
#include "Plugins/PacketLogger.h"
#include "Plugins/ThreadsafePacketLogger.h"

#include "ConnectionWaits.h"
#include "GetTime.h"
#include "InternalPacket.h"
#include "MessageIdentifiers.h"
#include "PeerScope.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include "DiagStall.h" // [DEBUG-stall]
#if defined( _WIN32 )
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <cstdio>
#include <cstdlib>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/*
PacketLogger::UserIDTOString, driven directly. It takes only an id and touches
nothing else on the plugin - no peer, no socket, no waiting - so exercising it is a
function call, which is why this case carries no [network] tag. It is protected and
virtual because the header's own comment says users should override it, so the test
reaches it the way a user would: through a subclass, calling the base implementation
that everyone who does not override it gets.

The regression this covers: the body wrote `res.ptr = '\0'` instead of
`*res.ptr = '\0'`, assigning the past-the-end pointer rather than terminating the
buffer - which MSVC accepts without /permissive- and GCC rejects outright. The
buffer is `static char str[256]`, zero-initialised once and never terminated again,
so any single call in isolation looks correct; it only shows up when a shorter id
follows a longer one and reads the previous call's tail back.
*/

using namespace RakNet;

namespace {

// UserIDTOString is protected, and overriding it is the documented use of this
// class. This one deliberately does not override it - it only widens access to the
// base version under test.
class ExposedPacketLogger : public PacketLogger
{
public:
    using PacketLogger::UserIDTOString;
};

} // namespace

TEST_CASE( "PacketLogger::UserIDTOString terminates its buffer between calls", "[packetlogger]" )
{
    ExposedPacketLogger logger;

    // The widest an unsigned char gets, and the setup for the call that follows.
    // Correct either way, and not only because the buffer starts zeroed: no id
    // renders wider than three digits, so nothing an earlier call anywhere in the
    // process could have left behind reaches past what this one overwrites.
    //
    // Both results are copied out before the next call, because the return points
    // at the static buffer that call reuses.
    const std::string threeDigits( logger.UserIDTOString( 255 ) );
    CHECK( threeDigits == "255" );

    // One digit over the same buffer. Without the terminator str[1..2] still hold
    // '5','5' and this comes back as "755".
    const std::string oneDigit( logger.UserIDTOString( 7 ) );
    CHECK( oneDigit == "7" );
}

/*
PacketLogger::OnInternalPacket, driven directly with a hand-built, unsplit
InternalPacket. Attaching the logger to a RakPeer that was never started sets
rakPeerInterface, and GetExternalID then just misses the published view, so no socket
is needed. The line is captured by overriding WriteLog.

The packet claims fewer bytes than its buffer holds, and the buffer carries a
distinctive ID straight after the time. Reading the ID of a packet too short to have
one then picks up that byte instead of faulting, so an over-read shows in the line.
*/

namespace {

class CapturingPacketLogger : public PacketLogger
{
public:
    void WriteLog( const char* str ) override { lines.emplace_back( str ); }

    std::vector<std::string> lines;
};

// Fields of a receive line from its direction onward: direction, type, reliable#, frame, ID.
// The clock column before it is locale formatted, so it is skipped by finding the
// direction rather than by counting commas.
std::vector<std::string> FieldsOfReceiveLine( const std::string& line )
{
    std::vector<std::string> fields;
    size_t start = line.find( ",Rcv," );
    if( start == std::string::npos )
        return fields;
    ++start;
    while( fields.size() < 5 )
    {
        const size_t end = line.find( ',', start );
        if( end == std::string::npos )
            break;
        fields.push_back( line.substr( start, end - start ) );
        start = end + 1;
    }
    return fields;
}

std::vector<std::string> LogTimestampedMessage( unsigned int claimedBytes )
{
    constexpr unsigned char kDistinctiveID = ID_USER_PACKET_ENUM;

    unsigned char buffer[16] = {};
    buffer[0] = ID_TIMESTAMP;
    for( size_t i = 0; i < sizeof( RakNet::Time ); ++i )
        buffer[1 + i] = static_cast<unsigned char>( 0x10 + i );
    buffer[1 + sizeof( RakNet::Time )] = kDistinctiveID;

    InternalPacket packet{};
    packet.data = buffer;
    packet.dataBitLength = BytesToBits( claimedBytes );
    packet.reliability = UNRELIABLE;
    packet.allocationScheme = InternalPacket::STACK;

    CapturingPacketLogger logger;
    PeerScope peers;
    RakPeerInterface* peer = peers.Create();
    peer->AttachPlugin( &logger );

    logger.OnInternalPacket( &packet, 0, UNASSIGNED_SYSTEM_ADDRESS, 0, 0 );
    peer->DetachPlugin( &logger );

    REQUIRE( logger.lines.size() == 1 );
    return FieldsOfReceiveLine( logger.lines[0] );
}

} // namespace

TEST_CASE( "PacketLogger logs a Timestamped Message with no room for an ID as ID_TIMESTAMP", "[packetlogger]" )
{
    // ID_TIMESTAMP and the full time, no ID. The distinctive ID sits one byte past
    // the packet.
    const std::vector<std::string> fields = LogTimestampedMessage( 1 + sizeof( RakNet::Time ) );

    REQUIRE( fields.size() == 5 );
    CHECK( fields[1] == "Nrm" );
    CHECK( fields[4] == "ID_TIMESTAMP" );
}

TEST_CASE( "PacketLogger logs a Timestamped Message by the ID after its time", "[packetlogger]" )
{
    const std::vector<std::string> fields = LogTimestampedMessage( 1 + sizeof( RakNet::Time ) + 1 );

    REQUIRE( fields.size() == 5 );
    CHECK( fields[1] == "Tms" );
    CHECK( fields[4] == std::to_string( ID_USER_PACKET_ENUM ) );
}

/*
A 255-byte prefix and suffix, the most SetPrefix and SetSuffix keep, logged through
OnInternalPacket. The whole line, from the clock to the suffix, must reach WriteLog.
*/

TEST_CASE( "PacketLogger logs a line with the longest prefix and suffix", "[packetlogger]" )
{
    const std::string prefix( 255, 'p' );
    const std::string suffix( 255, 's' );

    unsigned char buffer[1] = { ID_USER_PACKET_ENUM };
    InternalPacket packet{};
    packet.data = buffer;
    packet.dataBitLength = BytesToBits( 1 );
    packet.reliability = RELIABLE_ORDERED;
    packet.allocationScheme = InternalPacket::STACK;

    CapturingPacketLogger logger;
    logger.SetPrefix( prefix.c_str() );
    logger.SetSuffix( suffix.c_str() );
    PeerScope peers;
    RakPeerInterface* peer = peers.Create();
    peer->AttachPlugin( &logger );

    logger.OnInternalPacket( &packet, 0, UNASSIGNED_SYSTEM_ADDRESS, 0, 1 );
    peer->DetachPlugin( &logger );

    REQUIRE( logger.lines.size() == 1 );
    const std::string& line = logger.lines[0];
    CHECK( line.find( "," + prefix + "Snd," ) != std::string::npos );
    REQUIRE( line.size() > suffix.size() + 1 );
    CHECK( line.compare( line.size() - suffix.size() - 1, suffix.size() + 1, suffix + "," ) == 0 );
}

TEST_CASE( "PacketLogger::WriteMiscellaneous logs the formatted line", "[packetlogger]" )
{
    CapturingPacketLogger logger;
    PeerScope peers;
    RakPeerInterface* peer = peers.Create();
    peer->AttachPlugin( &logger );

    logger.WriteMiscellaneous( "Note", "hello" );
    peer->DetachPlugin( &logger );

    // Clock,Lcl,Note,,,,,Time,Local IP:Port,,,,,,,hello
    REQUIRE( logger.lines.size() == 1 );
    const std::string& line = logger.lines[0];
    const size_t type = line.find( ",Lcl,Note,,,,," );
    REQUIRE( type != std::string::npos );
    CHECK( type > 0 );

    const size_t timeStart = type + std::string( ",Lcl,Note,,,,," ).size();
    const size_t timeEnd = line.find( ',', timeStart );
    REQUIRE( timeEnd != std::string::npos );
    CHECK( timeEnd > timeStart );
    CHECK( line.find_first_not_of( "0123456789", timeStart ) == timeEnd );

    const size_t addressEnd = line.find( ',', timeEnd + 1 );
    REQUIRE( addressEnd != std::string::npos );
    CHECK( addressEnd > timeEnd + 1 );
    CHECK( line.substr( addressEnd ) == ",,,,,,,hello" );
}

/*
PacketLogger::OnAck, driven directly on a logger attached to an unstarted peer, with
SetPrintAcks off and on.
*/

namespace {

std::vector<std::string> LogAck( bool printAcks )
{
    CapturingPacketLogger logger;
    logger.SetPrintAcks( printAcks );
    PeerScope peers;
    RakPeerInterface* peer = peers.Create();
    peer->AttachPlugin( &logger );

    logger.OnAck( 7, UNASSIGNED_SYSTEM_ADDRESS, 0 );
    peer->DetachPlugin( &logger );
    return logger.lines;
}

} // namespace

TEST_CASE( "PacketLogger::OnAck logs nothing while SetPrintAcks is false", "[packetlogger]" )
{
    CHECK( LogAck( false ).empty() );
}

TEST_CASE( "PacketLogger::OnAck logs one ack line while SetPrintAcks is true", "[packetlogger]" )
{
    const std::vector<std::string> lines = LogAck( true );

    REQUIRE( lines.size() == 1 );
    INFO( lines[0] );
    CHECK( lines[0].find( ",Rcv,Ack," ) != std::string::npos );
}

/*
ThreadsafePacketLogger fed from two threads at once. The network thread logs the
offline pings a second peer keeps sending, and their pongs. Meanwhile the test thread
logs its own Ping calls, each of which runs OnDirectSocketSend on the caller's thread.
Those pings go to a peer that only the test thread addresses, so its send lines count
the Ping calls exactly. Update runs on the test thread, inside Receive.

The pinger can outrun the network thread, which then works off its backlog after the
test thread's last Receive, with no Update to drain the queue. That burst can pass the
default SetMaxQueuedLines cap, so the cap is lifted here: refusal has tests of its own.
*/

namespace {

class CollectingThreadsafePacketLogger : public ThreadsafePacketLogger
{
public:
    void WriteLog( const char* str ) override { lines.emplace_back( str ); }

    std::vector<std::string> lines;
};

// A raw line from its direction onward: direction, type, reliable#, frame, ID, bit
// length, time, local, remote, the four split and ordering fields, suffix, and the
// empty field after the trailing comma. Empty if the line has no Snd or Rcv direction.
std::vector<std::string> FieldsOfRawLine( const std::string& line )
{
    std::vector<std::string> fields;
    size_t start = line.find( ",Snd," );
    if( start == std::string::npos )
        start = line.find( ",Rcv," );
    if( start == std::string::npos )
        return fields;
    ++start;
    for( ;; )
    {
        const size_t end = line.find( ',', start );
        fields.push_back( line.substr( start, end == std::string::npos ? std::string::npos : end - start ) );
        if( end == std::string::npos )
            break;
        start = end + 1;
    }
    return fields;
}

} // namespace

TEST_CASE( "ThreadsafePacketLogger keeps every line logged from the network thread and the application thread at once", "[packetlogger][network]" )
{
    constexpr int kPings = 2000;

    CollectingThreadsafePacketLogger logger;
    logger.SetMaxQueuedLines( ( std::numeric_limits<unsigned int>::max )() );
    PeerScope peers;
    // Attached before Startup, because the network thread walks the plugin list unlocked.
    RakPeerInterface* logged = peers.Create();
    logged->AttachPlugin( &logger );
    SocketDescriptor socketDescriptor( 0, nullptr );
    REQUIRE( logged->Startup( 1, &socketDescriptor, 1 ) == RAKNET_STARTED );
    RakPeerInterface* pinger = peers.Client();
    RakPeerInterface* held = peers.Client();

    const unsigned short loggedPort = logged->GetMyBoundAddress().GetPort();
    const unsigned short heldPort = held->GetMyBoundAddress().GetPort();

    std::atomic<bool> stop{ false };
    std::thread pingLoop( [&] {
        while( !stop.load() )
        {
            pinger->Ping( "127.0.0.1", loggedPort, false );
            ConnectionWaits::Drain( pinger );
            std::this_thread::yield();
        }
    } );

    int pingsSent = 0;
    for( int i = 0; i < kPings; ++i )
    {
        if( logged->Ping( "127.0.0.1", heldPort, false ) )
            ++pingsSent;
        ConnectionWaits::Drain( logged );
        ConnectionWaits::Drain( held );
    }

    stop = true;
    pingLoop.join();
    // Stops the network thread, so the last Update collects every line.
    logged->Shutdown( 0 );
    logger.Update();
    REQUIRE( logger.GetLinesRefused() == 0 );

    const std::string heldSuffix = "|" + std::to_string( heldPort );
    int sendsToHeld = 0;
    int networkThreadLines = 0;
    for( const std::string& line : logger.lines )
    {
        const std::vector<std::string> fields = FieldsOfRawLine( line );
        INFO( line );
        REQUIRE( fields.size() == 15 );
        CHECK( fields[1] == "Raw" );
        CHECK( fields[14].empty() );

        const std::string& remote = fields[8];
        const bool toHeld = remote.size() > heldSuffix.size() && remote.compare( remote.size() - heldSuffix.size(), heldSuffix.size(), heldSuffix ) == 0;
        if( fields[0] == "Snd" && toHeld )
            ++sendsToHeld;
        else if( !toHeld )
            ++networkThreadLines;
    }

    CHECK( pingsSent == kPings );
    CHECK( sendsToHeld == pingsSent );
    CHECK( networkThreadLines > 0 );
}

/*
PacketLogger's setters called while the network thread logs. The test thread alternates
SetPrefix between two strings and toggles SetLogDirectMessages, SetPrintID and
SetPrintAcks, while a second peer's offline pings make the network thread log raw lines.
Every line's prefix must be exactly one of the two strings.
*/

namespace {

class LockedCapturingPacketLogger : public PacketLogger
{
public:
    void WriteLog( const char* str ) override
    {
        std::lock_guard<std::mutex> lock( linesMutex );
        lines.emplace_back( str );
    }

    size_t LineCount()
    {
        std::lock_guard<std::mutex> lock( linesMutex );
        return lines.size();
    }

    std::mutex linesMutex;
    std::vector<std::string> lines;
};

} // namespace

TEST_CASE( "PacketLogger's setters may be called while the network thread logs", "[packetlogger][network]" )
{
    constexpr size_t kLines = 500;
    const std::string longPrefix( 255, 'x' );
    const std::string shortPrefix = "y";

    LockedCapturingPacketLogger logger;
    logger.SetPrefix( shortPrefix.c_str() );
    PeerScope peers;
    // Attached before Startup, because the network thread walks the plugin list unlocked.
    RakPeerInterface* logged = peers.Create();
    logged->AttachPlugin( &logger );
    SocketDescriptor socketDescriptor( 0, nullptr );
    REQUIRE( logged->Startup( 1, &socketDescriptor, 1 ) == RAKNET_STARTED );
    RakPeerInterface* pinger = peers.Client();
    const unsigned short loggedPort = logged->GetMyBoundAddress().GetPort();

    for( int i = 0; i < 100000 && logger.LineCount() < kLines; ++i )
    {
        pinger->Ping( "127.0.0.1", loggedPort, false );
        ConnectionWaits::Drain( pinger );
        logger.SetPrefix( ( i % 2 == 0 ? longPrefix : shortPrefix ).c_str() );
        logger.SetLogDirectMessages( false );
        logger.SetLogDirectMessages( true );
        logger.SetPrintID( i % 2 == 0 );
        logger.SetPrintAcks( i % 2 == 0 );
    }

    // Stops the network thread, so no line is written while they are checked.
    logged->Shutdown( 0 );
    logged->DetachPlugin( &logger );

    REQUIRE( logger.lines.size() >= kLines );
    for( const std::string& line : logger.lines )
    {
        INFO( line );
        const size_t prefixStart = line.find( ',' );
        REQUIRE( prefixStart != std::string::npos );
        size_t prefixEnd = line.find( "Snd,", prefixStart + 1 );
        if( prefixEnd == std::string::npos )
            prefixEnd = line.find( "Rcv,", prefixStart + 1 );
        REQUIRE( prefixEnd != std::string::npos );
        const std::string prefix = line.substr( prefixStart + 1, prefixEnd - prefixStart - 1 );
        CHECK( ( prefix == longPrefix || prefix == shortPrefix ) );
    }
}

/*
PacketLogger::UserIDTOString reached from two threads at once. A connected client sends
Messages with a user ID, so the network thread logs each one it receives through
UserIDTOString. Meanwhile the test thread formats lines with a different user ID through
the public FormatLine. Each line must carry its own ID.
*/

namespace {

// Count of logger's lines whose ID field is id.
size_t CountLinesWithID( LockedCapturingPacketLogger& logger, const std::string& id )
{
    std::lock_guard<std::mutex> lock( logger.linesMutex );
    size_t count = 0;
    for( const std::string& line : logger.lines )
    {
        const std::vector<std::string> fields = FieldsOfRawLine( line );
        if( fields.size() > 4 && fields[4] == id )
            ++count;
    }
    return count;
}

} // namespace

TEST_CASE( "PacketLogger formats a user ID on the application thread while the network thread logs one", "[packetlogger][network]" )
{
    constexpr size_t kLines = 500;
    constexpr unsigned char kNetworkID = ID_USER_PACKET_ENUM;
    constexpr unsigned char kApplicationID = 250;

    LockedCapturingPacketLogger logger;
    logger.SetLogDirectMessages( false );
    PeerScope peers;
    // Attached before Startup, because the network thread walks the plugin list unlocked.
    RakPeerInterface* logged = peers.Create();
    logged->AttachPlugin( &logger );
    SocketDescriptor socketDescriptor( 0, nullptr );
    REQUIRE( logged->Startup( 1, &socketDescriptor, 1 ) == RAKNET_STARTED );
    logged->SetMaximumIncomingConnections( 1 );
    RakPeerInterface* sender = peers.Client();
    ConnectionWaits::ConnectAndWait( sender, logged );

    const std::string networkID = std::to_string( kNetworkID );
    const std::string applicationID = std::to_string( kApplicationID );
    const char message[] = { static_cast<char>( kNetworkID ), 'x' };
    size_t formatted = 0;
    size_t misformatted = 0;
    for( int i = 0; i < 100000 && CountLinesWithID( logger, networkID ) < kLines; ++i )
    {
        sender->Send( message, sizeof( message ), HIGH_PRIORITY, RELIABLE, 0, logged->GetMyGUID(), false );
        for( int j = 0; j < 10; ++j )
        {
            char line[1024];
            logger.FormatLine( line, "Snd", "Raw", 0, 0, kApplicationID, 0, 0, UNASSIGNED_SYSTEM_ADDRESS, UNASSIGNED_SYSTEM_ADDRESS, 0, 0, 0, 0 );
            const std::vector<std::string> fields = FieldsOfRawLine( line );
            ++formatted;
            if( fields.size() <= 4 || fields[4] != applicationID )
                ++misformatted;
        }
        ConnectionWaits::Drain( logged );
        ConnectionWaits::Drain( sender );
    }

    // Stops the network thread, so no line is written while they are checked.
    logged->Shutdown( 0 );
    logged->DetachPlugin( &logger );

    CHECK( CountLinesWithID( logger, networkID ) >= kLines );
    CHECK( formatted > 0 );
    CHECK( misformatted == 0 );
}

/*
PacketFileLogger::StartLog called while the network thread logs. A second peer's offline
pings make the network thread write raw lines, while the test thread starts one log and
then a second. Once the logger is destroyed, each file begins with the header, and the
second holds raw lines.
*/

namespace {

class CountingPacketFileLogger : public PacketFileLogger
{
public:
    void WriteLog( const char* str ) override
    {
        PacketFileLogger::WriteLog( str );
        ++linesLogged;
    }

    std::atomic<size_t> linesLogged{ 0 };
};

std::vector<std::string> ReadLines( const std::filesystem::path& path )
{
    std::vector<std::string> lines;
    std::ifstream in( path );
    std::string line;
    while( std::getline( in, line ) )
        lines.push_back( line );
    return lines;
}

// The one file in directory whose name starts with stem.
std::filesystem::path FileStartingWith( const std::filesystem::path& directory, const std::string& stem )
{
    std::filesystem::path found;
    for( const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator( directory ) )
    {
        if( entry.path().filename().string().rfind( stem, 0 ) == 0 )
        {
            CHECK( found.empty() );
            found = entry.path();
        }
    }
    return found;
}

// Removes every file in directory whose name starts with stem, on destruction, so a
// failed REQUIRE leaves none behind.
struct FilesRemover
{
    std::filesystem::path directory;
    std::string stem;

    ~FilesRemover()
    {
        std::error_code ignored;
        std::vector<std::filesystem::path> matches;
        for( const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator( directory, ignored ) )
        {
            if( entry.path().filename().string().rfind( stem, 0 ) == 0 )
                matches.push_back( entry.path() );
        }
        for( const std::filesystem::path& path : matches )
            std::filesystem::remove( path, ignored );
    }
};

} // namespace

TEST_CASE( "PacketFileLogger::StartLog may be called while the network thread logs", "[packetlogger][network]" )
{
    const std::filesystem::path directory = std::filesystem::temp_directory_path();
    const std::string stem = "PacketFileLoggerTest_" + std::to_string( RakNet::GetTimeUS() );
    const std::string firstPrefix = ( directory / ( stem + "_first" ) ).string();
    const std::string secondPrefix = ( directory / ( stem + "_second" ) ).string();
    // Declared before the logger, so its files are closed by the time they are removed.
    const FilesRemover remover{ directory, stem };

    {
        CountingPacketFileLogger logger;
        PeerScope peers;
        // Attached before Startup, because the network thread walks the plugin list unlocked.
        RakPeerInterface* logged = peers.Create();
        logged->AttachPlugin( &logger );
        SocketDescriptor socketDescriptor( 0, nullptr );
#if defined( RAKNET_DIAG_STALL )
        DiagStall::ResetSections(); // [DEBUG-stall]
        DiagStall::Reset( logged ); // [DEBUG-stall]
        const uint64_t diagStartupUs = RakNet::GetTimeUS();
#endif
        REQUIRE( logged->Startup( 1, &socketDescriptor, 1 ) == RAKNET_STARTED );
        RakPeerInterface* pinger = peers.Client();
        const unsigned short loggedPort = logged->GetMyBoundAddress().GetPort();
        int diagStage = 0; // [DEBUG-stall]

        // Pings until the network thread has logged count more lines.
        const auto pingUntilLogged = [&]( size_t count ) {
            const size_t target = logger.linesLogged + count;
            ++diagStage;
            const uint64_t diagStageStartUs = RakNet::GetTimeUS(); // [DEBUG-stall]
            int i = 0;
            // RN_DIAG_FORCE_STALL exercises the dump path locally.
            const int diagCap = std::getenv( "RN_DIAG_FORCE_STALL" ) ? 0 : 10000;
            for( ; i < diagCap && logger.linesLogged < target; ++i )
            {
                pinger->Ping( "127.0.0.1", loggedPort, false );
                ConnectionWaits::Drain( pinger );
                std::this_thread::yield();
            }
#if defined( RAKNET_DIAG_STALL )
            {
                using namespace DiagStall;
                const uint64_t now = RakNet::GetTimeUS();
                const bool met = logger.linesLogged >= target;
                printf( "[DEBUG-stall] stage=%d %s iters=%d stageUs=%llu lines=%zu cycles=%llu processed=%llu recvPushed=%llu maxCycleUs=%llu maxWaitUs=%llu\n", diagStage, met ? "OK" : "STALL", i,
                        (unsigned long long)( now - diagStageStartUs ), (size_t)logger.linesLogged, (unsigned long long)cycles.load(), (unsigned long long)datagramsProcessed.load(),
                        (unsigned long long)recvPushed.load(), (unsigned long long)maxCycleUs.load(), (unsigned long long)maxWaitUs.load() );
                {
                    const int sec = currentSection.load();
                    printf( "[DEBUG-stall] stage=%d section now=%s for %lluus;", diagStage, SectionName( sec ), sec != SEC_NONE && currentSectionStartUs.load() <= now ? (unsigned long long)( now - currentSectionStartUs.load() ) : 0ull );
                    for( int s = 1; s < SEC_COUNT; ++s )
                        printf( " %s max=%lluus n=%llu;", SectionName( s ), (unsigned long long)sectionMaxUs[s].load(), (unsigned long long)sectionCount[s].load() );
                    printf( "\n" );
                }
                if( !met )
                {
                    printf( "[DEBUG-stall] now=+%lluus since Startup; last=%s at +%lluus\n", (unsigned long long)( now - diagStartupUs ), Name( lastCheckpoint.load() ),
                            lastCheckpointUs.load() ? (unsigned long long)( lastCheckpointUs.load() - diagStartupUs ) : 0ull );
                    for( int cp = 1; cp < DS_COUNT; ++cp )
                        printf( "[DEBUG-stall]   %-22s first=+%lluus hits=%llu\n", Name( cp ), firstUs[cp].load() ? (unsigned long long)( firstUs[cp].load() - diagStartupUs ) : 0ull, (unsigned long long)hits[cp].load() );
                    printf( "[DEBUG-stall] recv first=+%lluus last=+%lluus\n", recvFirstUs.load() ? (unsigned long long)( recvFirstUs.load() - diagStartupUs ) : 0ull,
                            recvLastUs.load() ? (unsigned long long)( recvLastUs.load() - diagStartupUs ) : 0ull );
#if defined( _WIN32 )
                    // Does the stuck thread get CPU? Sample its cycles and checkpoint before and
                    // after 200 ms in which this thread stops flooding.
                    HANDLE th = updateThreadId.load() ? OpenThread( THREAD_QUERY_LIMITED_INFORMATION, FALSE, updateThreadId.load() ) : NULL;
                    ULONG64 c0 = 0, c1 = 0;
                    if( th )
                        QueryThreadCycleTime( th, &c0 );
                    const int cpBefore = lastCheckpoint.load();
                    const uint64_t hitsBefore = hits[DS_AFTER_WAIT].load();
                    Sleep( 200 );
                    if( th )
                        QueryThreadCycleTime( th, &c1 );
                    printf( "[DEBUG-stall] threadId=%lu cycles %llu -> %llu (+%llu) over 200ms quiet; checkpoint %s -> %s; AFTER_WAIT hits %llu -> %llu; lines=%zu\n", updateThreadId.load(),
                            (unsigned long long)c0, (unsigned long long)c1, (unsigned long long)( c1 - c0 ), Name( cpBefore ), Name( lastCheckpoint.load() ), (unsigned long long)hitsBefore,
                            (unsigned long long)hits[DS_AFTER_WAIT].load(), (size_t)logger.linesLogged );
                    printf( "[DEBUG-stall] after quiet: section=%s for %lluus\n", SectionName( currentSection.load() ),
                            currentSection.load() != SEC_NONE ? (unsigned long long)( RakNet::GetTimeUS() - currentSectionStartUs.load() ) : 0ull );
                    SYSTEM_INFO si;
                    GetSystemInfo( &si );
                    printf( "[DEBUG-stall] processors=%lu\n", si.dwNumberOfProcessors );
                    if( th )
                        CloseHandle( th );
#endif
                    fflush( stdout );
                }
            }
#endif
            REQUIRE( logger.linesLogged >= target );
        };

        pingUntilLogged( 20 );
        logger.StartLog( firstPrefix.c_str() );
        pingUntilLogged( 20 );
        logger.StartLog( secondPrefix.c_str() );
        pingUntilLogged( 20 );
        logged->Shutdown( 0 );
        logged->DetachPlugin( &logger );
    }

    const std::filesystem::path firstFile = FileStartingWith( directory, stem + "_first" );
    const std::filesystem::path secondFile = FileStartingWith( directory, stem + "_second" );
    REQUIRE( !firstFile.empty() );
    REQUIRE( !secondFile.empty() );
    const std::vector<std::string> firstLines = ReadLines( firstFile );
    const std::vector<std::string> secondLines = ReadLines( secondFile );

    REQUIRE( !firstLines.empty() );
    CHECK( firstLines[0].rfind( "Clock,", 0 ) == 0 );
    REQUIRE( !secondLines.empty() );
    CHECK( secondLines[0].rfind( "Clock,", 0 ) == 0 );
    size_t secondFileLines = 0;
    for( const std::string& line : secondLines )
    {
        if( FieldsOfRawLine( line ).size() == 15 )
            ++secondFileLines;
    }
    CHECK( secondFileLines > 0 );
}

/*
ThreadsafePacketLogger with its queue capped and Receive never called on the logged
peer, so nothing drains it. A second peer keeps sending offline pings until the cap
refuses a line. Every line offered to the queue is counted on the way in, so the
refused count can be checked against what was offered.
*/

namespace {

class CountingThreadsafePacketLogger : public CollectingThreadsafePacketLogger
{
public:
    std::atomic<uint64_t> linesOffered{ 0 };

protected:
    void AddToLog( const char* str ) override
    {
        ++linesOffered;
        CollectingThreadsafePacketLogger::AddToLog( str );
    }
};

constexpr unsigned int kQueueCap = 16;

// Fills logger's queue past kQueueCap, stops the network thread, then drains once.
void FillPastCapThenDrain( CountingThreadsafePacketLogger& logger )
{
    REQUIRE( logger.GetMaxQueuedLines() == 8192 );
    logger.SetMaxQueuedLines( kQueueCap );
    REQUIRE( logger.GetMaxQueuedLines() == kQueueCap );
    PeerScope peers;
    RakPeerInterface* logged = peers.Create();
    logged->AttachPlugin( &logger );
    SocketDescriptor socketDescriptor( 0, nullptr );
    REQUIRE( logged->Startup( 1, &socketDescriptor, 1 ) == RAKNET_STARTED );
    RakPeerInterface* pinger = peers.Client();
    const unsigned short loggedPort = logged->GetMyBoundAddress().GetPort();

    for( int i = 0; i < 10000 && logger.GetLinesRefused() == 0; ++i )
    {
        pinger->Ping( "127.0.0.1", loggedPort, false );
        ConnectionWaits::Drain( pinger );
        std::this_thread::yield();
    }
    REQUIRE( logger.GetLinesRefused() > 0 );

    logged->Shutdown( 0 );
    REQUIRE( logger.lines.empty() );
    logger.Update();
    logged->DetachPlugin( &logger );
}

} // namespace

TEST_CASE( "ThreadsafePacketLogger refuses lines past SetMaxQueuedLines and counts them", "[packetlogger][network]" )
{
    CountingThreadsafePacketLogger logger;
    FillPastCapThenDrain( logger );

    // The queued lines and the marker after them.
    CHECK( logger.lines.size() == kQueueCap + 1 );
    CHECK( logger.GetLinesRefused() == logger.linesOffered - kQueueCap );
}

TEST_CASE( "ThreadsafePacketLogger's Update ends a drain with the count it refused", "[packetlogger][network]" )
{
    CountingThreadsafePacketLogger logger;
    FillPastCapThenDrain( logger );

    REQUIRE( !logger.lines.empty() );
    const std::string& marker = logger.lines.back();
    INFO( marker );
    CHECK( marker.find( std::to_string( logger.GetLinesRefused() ) + " log lines refused at the cap" ) != std::string::npos );

    // The count resets with the drain.
    logger.lines.clear();
    logger.Update();
    CHECK( logger.lines.empty() );
}
