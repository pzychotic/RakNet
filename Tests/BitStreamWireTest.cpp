#include "BitStream.h"
#include "RakMemoryOverride.h"
#include "StringCompressorScope.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

/*
The bytes BitStream writes for multi-byte values and for the range-limited encodings.
RakNet 4.x peers read these bytes, so every expectation here is the exact encoding.
__BITSTREAM_NATIVE_END is not defined, so a little-endian host reverses every multi-byte
value into network order through ReverseBytes, and a big-endian host writes it as is.

The range-limited writers clamp before they encode. Each is pinned below, at and above
its range ends; the asserts in the writers bound how far outside a test may go.
*/

using namespace RakNet;

namespace {

std::vector<int> Bytes( const BitStream& bs )
{
    return std::vector<int>( bs.GetData(), bs.GetData() + bs.GetNumberOfBytesUsed() );
}

template<class T>
std::vector<int> Written( const T& value )
{
    BitStream bs;
    bs.Write( value );
    return Bytes( bs );
}

template<class T>
std::vector<int> WrittenCompressed( const T& value )
{
    BitStream bs;
    bs.WriteCompressed( value );
    return Bytes( bs );
}

std::vector<int> WrittenFloat16( float value, float floatMin, float floatMax )
{
    BitStream bs;
    bs.WriteFloat16( value, floatMin, floatMax );
    return Bytes( bs );
}

float ReadFloat16( uint16_t percentile, float floatMin, float floatMax )
{
    BitStream bs;
    bs.Write( percentile );
    float read = 12345.0f;
    REQUIRE( bs.ReadFloat16( read, floatMin, floatMax ) );
    return read;
}

std::vector<int> WrittenOrthMatrix( float m00, float m11, float m22 )
{
    BitStream bs;
    bs.WriteOrthMatrix( m00, 0.0f, 0.0f, 0.0f, m11, 0.0f, 0.0f, 0.0f, m22 );
    return Bytes( bs );
}

} // namespace

TEST_CASE( "ReverseBytes copies a buffer in reverse order", "[bitstream]" )
{
    unsigned char in[5] = { 1, 2, 3, 4, 5 };
    unsigned char out[5] = {};

    BitStream::ReverseBytes( in, out, 5 );
    CHECK( std::vector<int>( out, out + 5 ) == std::vector<int>{ 5, 4, 3, 2, 1 } );

    BitStream::ReverseBytes( in, out, 4 );
    CHECK( std::vector<int>( out, out + 5 ) == std::vector<int>{ 4, 3, 2, 1, 1 } );

    unsigned char untouched[1] = { 9 };
    BitStream::ReverseBytes( in, untouched, 0 );
    CHECK( untouched[0] == 9 );
}

TEST_CASE( "ReverseBytesInPlace reverses odd and even lengths and leaves the rest", "[bitstream]" )
{
    unsigned char odd[6] = { 1, 2, 3, 4, 5, 6 };
    BitStream::ReverseBytesInPlace( odd, 5 );
    CHECK( std::vector<int>( odd, odd + 6 ) == std::vector<int>{ 5, 4, 3, 2, 1, 6 } );

    unsigned char even[4] = { 1, 2, 3, 4 };
    BitStream::ReverseBytesInPlace( even, 4 );
    CHECK( std::vector<int>( even, even + 4 ) == std::vector<int>{ 4, 3, 2, 1 } );

    unsigned char one[1] = { 7 };
    BitStream::ReverseBytesInPlace( one, 1 );
    CHECK( one[0] == 7 );
    BitStream::ReverseBytesInPlace( one, 0 );
    CHECK( one[0] == 7 );
}

TEST_CASE( "Multi-byte values go out in network order", "[bitstream]" )
{
    CHECK( Written( uint16_t( 0x1234 ) ) == std::vector<int>{ 0x12, 0x34 } );
    CHECK( Written( uint32_t( 0x12345678 ) ) == std::vector<int>{ 0x12, 0x34, 0x56, 0x78 } );
    CHECK( Written( uint64_t( 0x0102030405060708 ) ) == std::vector<int>{ 1, 2, 3, 4, 5, 6, 7, 8 } );
    CHECK( Written( int32_t( -2 ) ) == std::vector<int>{ 0xFF, 0xFF, 0xFF, 0xFE } );
    CHECK( Written( 1.0f ) == std::vector<int>{ 0x3F, 0x80, 0x00, 0x00 } );
    CHECK( Written( -2.5 ) == std::vector<int>{ 0xC0, 0x04, 0, 0, 0, 0, 0, 0 } );
}

TEST_CASE( "Multi-byte values written after a partial byte keep network order", "[bitstream]" )
{
    BitStream bs;
    bs.Write( true );
    bs.Write( true );
    bs.Write( true );
    bs.Write( uint32_t( 0x12345678 ) );

    // 111 then 0001 0010 0011 0100 0101 0110 0111 1000, five zero bits of padding.
    CHECK( Bytes( bs ) == std::vector<int>{ 0xE2, 0x46, 0x8A, 0xCF, 0x00 } );

    BitStream reader( bs.GetData(), bs.GetNumberOfBytesUsed(), false );
    bool pad = false;
    REQUIRE( reader.Read( pad ) );
    REQUIRE( reader.Read( pad ) );
    REQUIRE( reader.Read( pad ) );
    uint32_t read = 0;
    REQUIRE( reader.Read( read ) );
    CHECK( read == 0x12345678 );
}

TEST_CASE( "Multi-byte values read back from network order", "[bitstream]" )
{
    const unsigned char wire[] = { 0x12, 0x34, 0x12, 0x34, 0x56, 0x78, 1, 2, 3, 4, 5, 6, 7, 8,
                                   0x3F, 0x80, 0x00, 0x00, 0xC0, 0x04, 0, 0, 0, 0, 0, 0 };
    BitStream bs( const_cast<unsigned char*>( wire ), sizeof( wire ), false );

    uint16_t u16 = 0;
    uint32_t u32 = 0;
    uint64_t u64 = 0;
    float f = 0;
    double d = 0;
    REQUIRE( bs.Read( u16 ) );
    REQUIRE( bs.Read( u32 ) );
    REQUIRE( bs.Read( u64 ) );
    REQUIRE( bs.Read( f ) );
    REQUIRE( bs.Read( d ) );
    CHECK( u16 == 0x1234 );
    CHECK( u32 == 0x12345678 );
    CHECK( u64 == 0x0102030405060708 );
    CHECK( f == 1.0f );
    CHECK( d == -2.5 );
}

TEST_CASE( "WriteFloat16 clamps the percentile to the range", "[bitstream]" )
{
    // The assert allows 0.001 outside the range, which clamps to the end.
    CHECK( WrittenFloat16( -1.0005f, -1.0f, 1.0f ) == std::vector<int>{ 0x00, 0x00 } );
    CHECK( WrittenFloat16( -1.0f, -1.0f, 1.0f ) == std::vector<int>{ 0x00, 0x00 } );
    CHECK( WrittenFloat16( 0.0f, -1.0f, 1.0f ) == std::vector<int>{ 0x7F, 0xFF } );
    CHECK( WrittenFloat16( 1.0f, -1.0f, 1.0f ) == std::vector<int>{ 0xFF, 0xFF } );
    CHECK( WrittenFloat16( 1.0005f, -1.0f, 1.0f ) == std::vector<int>{ 0xFF, 0xFF } );
}

TEST_CASE( "ReadFloat16 clamps what it reads to the range", "[bitstream]" )
{
    CHECK( ReadFloat16( 0, -1.0f, 1.0f ) == -1.0f );
    CHECK( ReadFloat16( 65535, -1.0f, 1.0f ) == 1.0f );

    // -0.3f + 1.0f * ( 0.9f - -0.3f ) rounds to a float above 0.9f.
    CHECK( -0.3f + ( 65535 / 65535.0f ) * ( 0.9f - -0.3f ) > 0.9f );
    CHECK( ReadFloat16( 65535, -0.3f, 0.9f ) == 0.9f );

    // Nothing reads below the minimum: the minimum plus a non-negative product never
    // rounds down. A NaN is neither below nor above, and passes through.
    CHECK( std::isnan( ReadFloat16( 0, 0.0f, std::numeric_limits<float>::infinity() ) ) );
}

TEST_CASE( "WriteCompressed clamps a float to -1 and 1", "[bitstream]" )
{
    // The assert allows 0.01 outside the range, which clamps to the end.
    CHECK( WrittenCompressed( -1.005f ) == std::vector<int>{ 0x00, 0x00 } );
    CHECK( WrittenCompressed( -1.0f ) == std::vector<int>{ 0x00, 0x00 } );
    CHECK( WrittenCompressed( 0.0f ) == std::vector<int>{ 0x7F, 0xFF } );
    CHECK( WrittenCompressed( 1.0f ) == std::vector<int>{ 0xFF, 0xFF } );
    CHECK( WrittenCompressed( 1.005f ) == std::vector<int>{ 0xFF, 0xFF } );
}

TEST_CASE( "WriteCompressed clamps a double to -1 and 1", "[bitstream]" )
{
    CHECK( WrittenCompressed( -1.005 ) == std::vector<int>{ 0x00, 0x00, 0x00, 0x00 } );
    CHECK( WrittenCompressed( -1.0 ) == std::vector<int>{ 0x00, 0x00, 0x00, 0x00 } );
    CHECK( WrittenCompressed( 0.0 ) == std::vector<int>{ 0x80, 0x00, 0x00, 0x00 } );
    // From 1 up, ( 1 + 1 ) * 2^31 overflows the uint32_t it is cast to, so only the
    // low end is pinned.
    CHECK( WrittenCompressed( 0.5 ) == std::vector<int>{ 0xC0, 0x00, 0x00, 0x00 } );
}

TEST_CASE( "WriteAlignedBytesSafe writes the full length but at most the maximum bytes", "[bitstream]" )
{
    const char payload[] = { 'a', 'b', 'c', 'd' };

    BitStream under;
    under.WriteAlignedBytesSafe( payload, 3, 4 );
    BitStream at;
    at.WriteAlignedBytesSafe( payload, 4, 4 );
    BitStream over;
    over.WriteAlignedBytesSafe( payload, 4, 2 );

    CHECK( Bytes( under ) == std::vector<int>{ 0x00, 0x00, 0x00, 0x01, 0x80, 0x61, 0x62, 0x63 } );
    CHECK( Bytes( at ) == std::vector<int>{ 0x00, 0x00, 0x00, 0x02, 0x00, 0x61, 0x62, 0x63, 0x64 } );
    CHECK( Bytes( over ) == std::vector<int>{ 0x00, 0x00, 0x00, 0x02, 0x00, 0x61, 0x62 } );
}

TEST_CASE( "ReadAlignedBytesSafe reads at most the maximum bytes", "[bitstream]" )
{
    const char payload[] = { 'a', 'b', 'c', 'd' };

    for( unsigned int maxBytes : { 5u, 4u, 2u } )
    {
        BitStream bs;
        bs.WriteAlignedBytesSafe( payload, 4, 4 );

        char read[5] = { 'x', 'x', 'x', 'x', 'x' };
        unsigned int length = 0;
        REQUIRE( bs.ReadAlignedBytesSafe( read, length, maxBytes ) );
        CHECK( length == ( maxBytes < 4 ? maxBytes : 4 ) );
        CHECK( std::vector<char>( read, read + 5 ) == ( maxBytes < 4 ? std::vector<char>{ 'a', 'b', 'x', 'x', 'x' } : std::vector<char>{ 'a', 'b', 'c', 'd', 'x' } ) );

        BitStream allocated;
        allocated.WriteAlignedBytesSafe( payload, 4, 4 );
        char* buffer = nullptr;
        unsigned int allocatedLength = 0;
        REQUIRE( allocated.ReadAlignedBytesSafeAlloc( &buffer, allocatedLength, maxBytes ) );
        CHECK( allocatedLength == length );
        CHECK( std::vector<char>( buffer, buffer + allocatedLength ) == std::vector<char>( read, read + length ) );
        rakFree_Ex( buffer, _FILE_AND_LINE_ );
    }
}

TEST_CASE( "WriteOrthMatrix takes a negative trace sum as zero", "[bitstream]" )
{
    // Sign bits for w, x, y, z, then |x|, |y|, |z| as uint16 * 65535.
    // Identity: w's sum is 4, x's, y's and z's are exactly 0.
    CHECK( WrittenOrthMatrix( 1.0f, 1.0f, 1.0f ) == std::vector<int>{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } );
    // 180 degrees about z: z's sum is 4, the others exactly 0.
    CHECK( WrittenOrthMatrix( -1.0f, -1.0f, 1.0f ) == std::vector<int>{ 0x00, 0x00, 0x00, 0x00, 0x0F, 0xFF, 0xF0 } );
    // w's sum is -2, taken as 0; x's, y's and z's are 2.
    CHECK( WrittenOrthMatrix( -1.0f, -1.0f, -1.0f ) == std::vector<int>{ 0x0B, 0x50, 0x4B, 0x50, 0x4B, 0x50, 0x40 } );
}

TEST_CASE( "Types with their own encoding write fixed bytes", "[bitstream]" )
{
    const SystemAddress address( "10.0.0.1", 0x1234 );
    const RakNetGUID guid( 0x0102030405060708 );
    // The IP version, the address inverted bit by bit, then the port, all in network order.
    const std::vector<int> addressBytes{ 0x04, 0xF5, 0xFF, 0xFF, 0xFE, 0x12, 0x34 };
    const std::vector<int> guidBytes{ 1, 2, 3, 4, 5, 6, 7, 8 };
    // A uint24_t is byte-aligned and little-endian.
    const std::vector<int> uint24Bytes{ 0x56, 0x34, 0x12 };

    SECTION( "Write" )
    {
        CHECK( Written( true ) == std::vector<int>{ 0x80 } );
        CHECK( Written( false ) == std::vector<int>{ 0x00 } );
        CHECK( Written( address ) == addressBytes );
        CHECK( Written( uint24_t( 0x123456 ) ) == uint24Bytes );
        CHECK( Written( guid ) == guidBytes );
        CHECK( Written( std::string( "ab" ) ) == std::vector<int>{ 0x00, 0x02, 0x61, 0x62 } );

        BitStream bs;
        bs.Write( true );
        bs.Write( uint24_t( 0x123456 ) );
        CHECK( Bytes( bs ) == std::vector<int>{ 0x80, 0x56, 0x34, 0x12 } );
    }

    SECTION( "WriteCompressed" )
    {
        CHECK( WrittenCompressed( true ) == std::vector<int>{ 0x80 } );
        CHECK( WrittenCompressed( address ) == addressBytes );
        CHECK( WrittenCompressed( uint24_t( 0x123456 ) ) == uint24Bytes );
        CHECK( WrittenCompressed( guid ) == guidBytes );
        CHECK( WrittenCompressed( 0.5f ) == std::vector<int>{ 0xBF, 0xFF } );
        CHECK( WrittenCompressed( 0.5 ) == std::vector<int>{ 0xC0, 0x00, 0x00, 0x00 } );

        StringCompressorScope compressor;
        CHECK( WrittenCompressed( std::string( "ab" ) ) == std::vector<int>{ 0x00, 0x00, 0x00, 0x08, 0x31, 0xEA, 0x80 } );
    }

    SECTION( "A bool delta is the bool alone" )
    {
        BitStream delta;
        delta.WriteDelta( true, true );
        CHECK( Bytes( delta ) == std::vector<int>{ 0x80 } );

        BitStream compressedDelta;
        compressedDelta.WriteCompressedDelta( true, true );
        CHECK( Bytes( compressedDelta ) == std::vector<int>{ 0x80 } );

        BitStream compressedDeltaNoLast;
        compressedDeltaNoLast.WriteCompressedDelta( false );
        CHECK( Bytes( compressedDeltaNoLast ) == std::vector<int>{ 0x00 } );
    }
}

TEST_CASE( "Types with their own encoding read fixed bytes", "[bitstream]" )
{
    unsigned char trueWire[] = { 0x80 };
    unsigned char addressWire[] = { 0x04, 0xF5, 0xFF, 0xFF, 0xFE, 0x12, 0x34 };
    // A bit before the uint24_t, which the read skips to the byte boundary.
    unsigned char uint24Wire[] = { 0x80, 0x56, 0x34, 0x12 };
    unsigned char guidWire[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    unsigned char stringWire[] = { 0x00, 0x02, 0x61, 0x62 };
    unsigned char compressedStringWire[] = { 0x00, 0x00, 0x00, 0x08, 0x31, 0xEA, 0x80 };
    unsigned char floatWire[] = { 0xBF, 0xFF };
    unsigned char doubleWire[] = { 0xC0, 0x00, 0x00, 0x00 };

    SECTION( "Read" )
    {
        BitStream boolStream( trueWire, sizeof( trueWire ), false );
        bool b = false;
        REQUIRE( boolStream.Read( b ) );
        CHECK( b );

        BitStream addressStream( addressWire, sizeof( addressWire ), false );
        SystemAddress address;
        REQUIRE( addressStream.Read( address ) );
        CHECK( address == SystemAddress( "10.0.0.1", 0x1234 ) );
        CHECK( address.GetPort() == 0x1234 );

        BitStream uint24Stream( uint24Wire, sizeof( uint24Wire ), false );
        REQUIRE( uint24Stream.Read( b ) );
        uint24_t u24;
        REQUIRE( uint24Stream.Read( u24 ) );
        CHECK( u24.val == 0x123456 );

        BitStream guidStream( guidWire, sizeof( guidWire ), false );
        RakNetGUID guid;
        REQUIRE( guidStream.Read( guid ) );
        CHECK( guid.g == 0x0102030405060708 );

        BitStream stringStream( stringWire, sizeof( stringWire ), false );
        std::string s;
        REQUIRE( stringStream.Read( s ) );
        CHECK( s == "ab" );
    }

    SECTION( "ReadCompressed" )
    {
        BitStream boolStream( trueWire, sizeof( trueWire ), false );
        bool b = false;
        REQUIRE( boolStream.ReadCompressed( b ) );
        CHECK( b );

        BitStream addressStream( addressWire, sizeof( addressWire ), false );
        SystemAddress address;
        REQUIRE( addressStream.ReadCompressed( address ) );
        CHECK( address == SystemAddress( "10.0.0.1", 0x1234 ) );

        BitStream uint24Stream( uint24Wire, sizeof( uint24Wire ), false );
        REQUIRE( uint24Stream.Read( b ) );
        uint24_t u24;
        REQUIRE( uint24Stream.ReadCompressed( u24 ) );
        CHECK( u24.val == 0x123456 );

        BitStream guidStream( guidWire, sizeof( guidWire ), false );
        RakNetGUID guid;
        REQUIRE( guidStream.ReadCompressed( guid ) );
        CHECK( guid.g == 0x0102030405060708 );

        BitStream floatStream( floatWire, sizeof( floatWire ), false );
        float f = 0;
        REQUIRE( floatStream.ReadCompressed( f ) );
        CHECK( f == 0xBFFF / 32767.5f - 1.0f );

        BitStream doubleStream( doubleWire, sizeof( doubleWire ), false );
        double d = 0;
        REQUIRE( doubleStream.ReadCompressed( d ) );
        CHECK( d == 0.5 );

        StringCompressorScope compressor;
        BitStream stringStream( compressedStringWire, sizeof( compressedStringWire ), false );
        std::string s;
        REQUIRE( stringStream.ReadCompressed( s ) );
        CHECK( s == "ab" );
    }

    SECTION( "A bool delta is the bool alone" )
    {
        BitStream deltaStream( trueWire, sizeof( trueWire ), false );
        bool b = false;
        REQUIRE( deltaStream.ReadDelta( b ) );
        CHECK( b );
        CHECK( deltaStream.GetNumberOfUnreadBits() == 7 );

        BitStream compressedDeltaStream( trueWire, sizeof( trueWire ), false );
        b = false;
        REQUIRE( compressedDeltaStream.ReadCompressedDelta( b ) );
        CHECK( b );
        CHECK( compressedDeltaStream.GetNumberOfUnreadBits() == 7 );
    }
}
