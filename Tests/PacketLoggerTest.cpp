#include "Plugins/PacketLogger.h"

#include "InternalPacket.h"
#include "MessageIdentifiers.h"
#include "PeerScope.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

/*
PacketLogger::UserIDTOString, driven directly. It takes only an id and touches
nothing else on the plugin - no peer, no socket, no waiting - so exercising it is a
function call, which is why this file carries no [network] tag. It is protected and
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
