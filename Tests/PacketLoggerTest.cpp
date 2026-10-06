#include "Plugins/PacketLogger.h"
#include "Plugins/ThreadsafePacketLogger.h"

#include "InternalPacket.h"
#include "MessageIdentifiers.h"
#include "PeerScope.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
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
    packet.dataBitLength = BYTES_TO_BITS( claimedBytes );
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
    packet.dataBitLength = BYTES_TO_BITS( 1 );
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

void DrainReceive( RakPeerInterface* peer )
{
    for( Packet* packet = peer->Receive(); packet != nullptr; packet = peer->Receive() )
        peer->DeallocatePacket( packet );
}

} // namespace

TEST_CASE( "ThreadsafePacketLogger keeps every line logged from the network thread and the user thread at once", "[packetlogger][network]" )
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
            DrainReceive( pinger );
            std::this_thread::yield();
        }
    } );

    int pingsSent = 0;
    for( int i = 0; i < kPings; ++i )
    {
        if( logged->Ping( "127.0.0.1", heldPort, false ) )
            ++pingsSent;
        DrainReceive( logged );
        DrainReceive( held );
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
SetPrefix between two strings and toggles SetLogDirectMessages, while a second peer's
offline pings make the network thread log raw lines. Every line's prefix must be exactly
one of the two strings.
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
        DrainReceive( pinger );
        logger.SetPrefix( ( i % 2 == 0 ? longPrefix : shortPrefix ).c_str() );
        logger.SetLogDirectMessages( false );
        logger.SetLogDirectMessages( true );
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
        DrainReceive( pinger );
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
