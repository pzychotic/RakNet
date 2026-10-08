#include "TCPByteBuffer.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

/*
TCPByteBuffer holds the bytes TCPInterface has yet to send to a client and the bytes
PacketizedTCP has yet to frame into a message. It is pure memory: no sockets, nothing
that sleeps.
*/

using namespace RakNet;

namespace {

void Append( TCPByteBuffer& buffer, const std::string& text )
{
    buffer.Append( text.data(), text.size() );
}

std::string Read( TCPByteBuffer& buffer, size_t n )
{
    std::string out( n, '\0' );
    REQUIRE( buffer.Read( &out[0], n ) );
    return out;
}

} // namespace

TEST_CASE( "TCPByteBuffer reads back what several appends wrote, in order", "[tcpbytebuffer]" )
{
    TCPByteBuffer buffer;
    Append( buffer, "abc" );
    Append( buffer, "defg" );
    Append( buffer, "h" );

    CHECK( buffer.Size() == 8 );
    CHECK( Read( buffer, 2 ) == "ab" );
    CHECK( Read( buffer, 5 ) == "cdefg" );
    CHECK( buffer.Size() == 1 );
    CHECK( Read( buffer, 1 ) == "h" );
    CHECK( buffer.Size() == 0 );
}

TEST_CASE( "TCPByteBuffer refuses a Peek or Read of more than it holds and changes nothing", "[tcpbytebuffer]" )
{
    TCPByteBuffer buffer;
    Append( buffer, "xyz" );
    char out[4] = { '-', '-', '-', '-' };

    CHECK_FALSE( buffer.Peek( out, 4 ) );
    CHECK_FALSE( buffer.Read( out, 4 ) );

    CHECK( std::string( out, 4 ) == "----" );
    CHECK( buffer.Size() == 3 );
    CHECK( Read( buffer, 3 ) == "xyz" );
}

TEST_CASE( "TCPByteBuffer Peek leaves the bytes to be read again", "[tcpbytebuffer]" )
{
    TCPByteBuffer buffer;
    Append( buffer, "peek" );
    char out[2];

    REQUIRE( buffer.Peek( out, 2 ) );

    CHECK( std::string( out, 2 ) == "pe" );
    CHECK( Read( buffer, 4 ) == "peek" );
}

TEST_CASE( "TCPByteBuffer Contiguous returns everything after a consume and a further append", "[tcpbytebuffer]" )
{
    TCPByteBuffer buffer;
    // Fewer than half consumed, so the append lands behind bytes already read.
    Append( buffer, "0123456789" );
    buffer.Consume( 3 );
    Append( buffer, "abcdef" );

    size_t n = 0;
    const char* run = buffer.Contiguous( &n );

    REQUIRE( n == 13 );
    CHECK( std::string( run, n ) == "3456789abcdef" );
}

TEST_CASE( "TCPByteBuffer Contiguous of an empty buffer is zero bytes", "[tcpbytebuffer]" )
{
    TCPByteBuffer buffer;
    size_t n = 1;

    buffer.Contiguous( &n );

    CHECK( n == 0 );
}

TEST_CASE( "TCPByteBuffer keeps its contents in order while reads pass the middle of the buffer", "[tcpbytebuffer]" )
{
    // Reads of 5 against appends of 7 leave a little more behind each round, so the read
    // offset passes half of what is stored again and again.
    TCPByteBuffer buffer;
    std::string expected;
    char next = 0;
    for( int round = 0; round < 200; round++ )
    {
        std::string chunk;
        for( int i = 0; i < 7; i++ )
            chunk += next++;
        Append( buffer, chunk );
        expected += chunk;

        CHECK( Read( buffer, 5 ) == expected.substr( 0, 5 ) );
        expected.erase( 0, 5 );
        buffer.Consume( 1 );
        expected.erase( 0, 1 );

        size_t n = 0;
        const char* run = buffer.Contiguous( &n );
        REQUIRE( std::string( run, n ) == expected );
    }
}

TEST_CASE( "TCPByteBuffer Clear empties it for reuse", "[tcpbytebuffer]" )
{
    TCPByteBuffer buffer;
    Append( buffer, "stale" );
    buffer.Consume( 2 );

    buffer.Clear();

    CHECK( buffer.Size() == 0 );
    Append( buffer, "fresh" );
    CHECK( Read( buffer, 5 ) == "fresh" );
}
