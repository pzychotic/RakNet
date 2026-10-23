#include "BitStream.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstdint>

/*
Write( BitStream*, numberOfBits ) appends bits read from another BitStream, at any bit
offset in either stream. The copied bits must match the source bit for bit, the bits
already in the destination must survive, and nothing past the copied bits may leak into
what is written next. Ones on both sides of the copy show a bit OR-ed into the wrong place.
*/

using namespace RakNet;

namespace {

constexpr unsigned int kSourceBytes = 40;

unsigned char SourceByte( unsigned int i )
{
    return (unsigned char)( i * 37u + 11u );
}

bool SourceBit( BitSize_t i )
{
    return ( SourceByte( i >> 3 ) & ( 0x80 >> ( i & 7 ) ) ) != 0;
}

void FillSource( BitStream& source )
{
    for( unsigned int i = 0; i < kSourceBytes; i++ )
        source.Write( SourceByte( i ) );
}

void WriteOnes( BitStream& bs, BitSize_t count )
{
    for( BitSize_t i = 0; i < count; i++ )
        bs.Write1();
}

void CheckOnes( BitStream& bs, BitSize_t count )
{
    for( BitSize_t i = 0; i < count; i++ )
        REQUIRE( bs.ReadBit() );
}

} // namespace

TEST_CASE( "Write( BitStream* ) copies every bit at any offset in either stream", "[bitstream]" )
{
    const BitSize_t sourceOffset = GENERATE( 0u, 8u, 16u, 3u, 13u );
    const BitSize_t destinationOffset = GENERATE( 0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u );
    const BitSize_t count = GENERATE( 0u, 1u, 7u, 8u, 9u, 15u, 16u, 17u, 64u, 250u );

    BitStream source;
    FillSource( source );
    source.SetReadOffset( sourceOffset );
    REQUIRE( count <= source.GetNumberOfUnreadBits() );

    BitStream destination;
    WriteOnes( destination, destinationOffset );
    destination.Write( &source, count );
    WriteOnes( destination, 8 );
    destination.Write( (uint32_t)0 );

    CHECK( source.GetReadOffset() == sourceOffset + count );
    REQUIRE( destination.GetNumberOfBitsUsed() == destinationOffset + count + 8 + 32 );

    CheckOnes( destination, destinationOffset );
    for( BitSize_t i = 0; i < count; i++ )
        REQUIRE( destination.ReadBit() == SourceBit( sourceOffset + i ) );
    CheckOnes( destination, 8 );
    uint32_t trailing = 1;
    REQUIRE( destination.Read( trailing ) );
    CHECK( trailing == 0 );
}

TEST_CASE( "Write( BitStream* ) copies no more than the source has unread", "[bitstream]" )
{
    const BitSize_t sourceOffset = GENERATE( 0u, 8u, 3u );
    const BitSize_t destinationOffset = GENERATE( 0u, 5u );

    BitStream source;
    FillSource( source );
    source.SetReadOffset( sourceOffset );
    const BitSize_t unread = source.GetNumberOfUnreadBits();

    BitStream destination;
    WriteOnes( destination, destinationOffset );
    destination.Write( &source, unread + 100 );

    CHECK( source.GetReadOffset() == source.GetNumberOfBitsUsed() );
    REQUIRE( destination.GetNumberOfBitsUsed() == destinationOffset + unread );
    CheckOnes( destination, destinationOffset );
    for( BitSize_t i = 0; i < unread; i++ )
        REQUIRE( destination.ReadBit() == SourceBit( sourceOffset + i ) );
}
