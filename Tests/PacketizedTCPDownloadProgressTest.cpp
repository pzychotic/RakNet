#include "MessageIdentifiers.h"
#include "PacketizedTCP.h"
#include "RakAssert.h"
#include "RakMemoryOverride.h"
#include "RakNetTypes.h"
#include "SocketDefines.h"
#include "SocketIncludes.h"
#include "WinsockScope.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

/*
Pins that PacketizedTCP's ID_DOWNLOAD_PROGRESS reports only bytes it buffered.

A partial message whose buffered byte count crosses a multiple of 65536 queues a progress
Packet carrying the message's first chunk, peeked from the buffer after the 4-byte header.
At the first crossing as few as 65532 bytes follow the header, so the chunk is reported as
long as the peek copied: partLength, length and bitSize all describe that count, and no
byte of the Packet is left unwritten.

The client is a raw socket, as in TCPCapsTest.cpp, so it can send a header announcing more
than it then sends.
*/

using namespace RakNet;

namespace {

// TCP, past every port another TCPInterface test takes. CreateListenSocket does not set
// SO_REUSEADDR before it binds, so no port is shared between cases.
constexpr unsigned short kShortFirstChunkListenPort = 31056;

// Loopback, so every wait here is over as soon as the threads have been scheduled once.
// Generous so a loaded machine cannot turn a pass into a failure.
constexpr std::chrono::milliseconds kDeadline( 5000 );

// Every rakMalloc_Ex block starts as this, so a byte never written shows up as it.
constexpr unsigned char kFill = 0xA5;

// Runs until the predicate holds or the deadline passes; returns whether it held.
template <typename Predicate>
bool WaitFor( Predicate predicate )
{
    const auto deadline = std::chrono::steady_clock::now() + kDeadline;

    while( std::chrono::steady_clock::now() < deadline )
    {
        if( predicate() )
            return true;

        std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
    }

    return predicate();
}

// Fills every rakMalloc_Ex block with kFill while in scope, so an uninitialised read does
// not sample zero.
//
// The hook is a plain global that TCPInterface's thread calls, so a case declares its
// FilledMalloc before its PacketizedTCP: it goes in before Start creates that thread and
// comes out after Stop has waited for it.
class FilledMalloc
{
public:
    FilledMalloc()
    {
        RakAssert( s_previous == 0 );
        s_previous = GetMalloc_Ex();
        SetMalloc_Ex( &Malloc );
    }

    ~FilledMalloc()
    {
        SetMalloc_Ex( s_previous );
        s_previous = 0;
    }

    FilledMalloc( const FilledMalloc& ) = delete;
    FilledMalloc& operator=( const FilledMalloc& ) = delete;

private:
    static void* Malloc( size_t size, const char* file, unsigned int line )
    {
        void* p = s_previous( size, file, line );
        if( p != 0 )
            memset( p, kFill, size );
        return p;
    }

    static void* ( *s_previous )( size_t size, const char* file, unsigned int line );
};

void* ( *FilledMalloc::s_previous )( size_t size, const char* file, unsigned int line ) = 0;

// A blocking TCP connection to the listener on loopback.
__TCPSOCKET__ Connect( unsigned short listenPort )
{
    const __TCPSOCKET__ s = socket__( AF_INET, SOCK_STREAM, IPPROTO_TCP );
    REQUIRE( s != (__TCPSOCKET__)-1 );

    sockaddr_in remote;
    memset( &remote, 0, sizeof( remote ) );
    remote.sin_family = AF_INET;
    remote.sin_port = htons( listenPort );
    remote.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
    REQUIRE( connect__( s, (const sockaddr*)&remote, sizeof( remote ) ) == 0 );
    return s;
}

void SendAll( __TCPSOCKET__ s, const char* data, size_t length )
{
    while( length != 0 )
    {
        const int sent = send__( s, data, (int)length, 0 );
        REQUIRE( sent > 0 );
        data += sent;
        length -= (size_t)sent;
    }
}

// Below 0x80, so never kFill.
unsigned char PatternByte( size_t offset )
{
    return (unsigned char)( ( offset * 7 + 1 ) & 0x7F );
}

} // namespace

TEST_CASE( "PacketizedTCP's first ID_DOWNLOAD_PROGRESS reports only the bytes buffered after the header", "[packetizedtcp][network]" )
{
    WinsockScope winsock;
    FilledMalloc filledMalloc;

    PacketizedTCP server;
    REQUIRE( server.Start( kShortFirstChunkListenPort, 1 ) );

    const __TCPSOCKET__ client = Connect( kShortFirstChunkListenPort );
    REQUIRE( WaitFor( [&] { return server.HasNewIncomingConnection() != UNASSIGNED_SYSTEM_ADDRESS; } ) );

    // A header announcing 200000 bytes, then 65532 of them: 65536 buffered in all, so the
    // first 65536-byte boundary is crossed on the last byte however TCP segments the stream.
    constexpr unsigned int kPayloadLength = 65532;
    const uint32_t header = htonl( 200000 );
    std::vector<char> stream( sizeof( header ) );
    memcpy( stream.data(), &header, sizeof( header ) );
    std::vector<unsigned char> payload( kPayloadLength );
    for( size_t i = 0; i < payload.size(); ++i )
        payload[i] = PatternByte( i );
    stream.insert( stream.end(), payload.begin(), payload.end() );
    SendAll( client, stream.data(), stream.size() );

    Packet* progress = 0;
    REQUIRE( WaitFor( [&] { return ( progress = server.Receive() ) != 0; } ) );

    constexpr unsigned int kPrefixLength = sizeof( MessageID ) + 3 * sizeof( unsigned int );
    REQUIRE( progress->length >= kPrefixLength );
    CHECK( progress->data[0] == ID_DOWNLOAD_PROGRESS );

    unsigned int partLength = 0;
    memcpy( &partLength, progress->data + sizeof( MessageID ) + 2 * sizeof( unsigned int ), sizeof( partLength ) );
    CHECK( partLength == kPayloadLength );
    CHECK( progress->length == kPrefixLength + kPayloadLength );
    CHECK( progress->bitSize == BYTES_TO_BITS( progress->length ) );

    // Compared over the whole reported length, so a chunk reported too long shows its fill.
    const std::vector<unsigned char> chunk( progress->data + kPrefixLength, progress->data + progress->length );
    const bool isChunkThePayload = chunk == payload;
    CHECK( isChunkThePayload );

    server.DeallocatePacket( progress );
    closesocket__( client );
    server.Stop();
}
