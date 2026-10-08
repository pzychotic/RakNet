#include "BitStream.h"
#include "RakNetTypes.h"
#include "SequenceRanges.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

/*
The run-length set of datagram sequence numbers that ReliabilityLayer puts in ACK and NAK
datagrams. RakNet 4.x peers read these bytes, so every expectation here is the exact
encoding: a byte-aligned uint16 count in network order, then per run a byte that is 1 for
a single number, the first number, and the last only when it differs. uint24_t goes out as
three little-endian bytes.
*/

using namespace RakNet;

namespace {

using Ranges = SequenceRanges<uint24_t>;

const BitSize_t unlimited = 1u << 20;

std::vector<unsigned char> Bytes( const BitStream& bs )
{
    return std::vector<unsigned char>( bs.GetData(), bs.GetData() + bs.GetNumberOfBytesUsed() );
}

std::vector<unsigned char> Serialized( Ranges& ranges, BitSize_t maxBits = unlimited, bool clearSerialized = false )
{
    BitStream bs;
    ranges.Serialize( &bs, maxBits, clearSerialized );
    return Bytes( bs );
}

void Insert( Ranges& ranges, std::initializer_list<uint32_t> numbers )
{
    for( uint32_t n : numbers )
        ranges.Insert( n );
}

unsigned char* Data( const std::vector<unsigned char>& bytes )
{
    return const_cast<unsigned char*>( bytes.data() );
}

} // namespace

TEST_CASE( "SequenceRanges serializes a single number as a flagged min", "[sequenceranges]" )
{
    Ranges ranges;
    ranges.Insert( 0x123456u );

    CHECK( ranges.Size() == 1 );
    CHECK( ranges.RangeSum() == 1 );
    CHECK( Serialized( ranges ) == std::vector<unsigned char>{ 0x00, 0x01, 0x01, 0x56, 0x34, 0x12 } );
}

TEST_CASE( "SequenceRanges serializes a run as min and max", "[sequenceranges]" )
{
    Ranges ranges;
    Insert( ranges, { 10, 11, 12, 13 } );

    CHECK( ranges.Size() == 1 );
    CHECK( ranges.RangeSum() == 4 );
    CHECK( Serialized( ranges ) == std::vector<unsigned char>{ 0x00, 0x01, 0x00, 10, 0, 0, 13, 0, 0 } );
}

TEST_CASE( "SequenceRanges serializes singles and runs in ascending order", "[sequenceranges]" )
{
    Ranges ranges;
    Insert( ranges, { 1, 5, 6, 7, 20, 300, 301 } );

    CHECK( ranges.Size() == 4 );
    CHECK( ranges.RangeSum() == 7 );
    CHECK( Serialized( ranges ) == std::vector<unsigned char>{
                                       0x00, 0x04,             //
                                       0x01, 1, 0, 0,          //
                                       0x00, 5, 0, 0, 7, 0, 0, //
                                       0x01, 20, 0, 0,         //
                                       0x00, 0x2C, 0x01, 0, 0x2D, 0x01, 0 } );
}

TEST_CASE( "SequenceRanges merges numbers inserted out of order and ignores repeats", "[sequenceranges]" )
{
    Ranges ranges;
    Insert( ranges, { 9, 7, 8, 4, 2, 8, 3, 9 } );

    CHECK( ranges.Size() == 2 );
    CHECK( ranges.RangeSum() == 6 );
    CHECK( Serialized( ranges ) == std::vector<unsigned char>{ 0x00, 0x02, 0x00, 2, 0, 0, 4, 0, 0, 0x00, 7, 0, 0, 9, 0, 0 } );
}

TEST_CASE( "SequenceRanges extends a run by one past its end when another run follows", "[sequenceranges]" )
{
    Ranges ranges;
    Insert( ranges, { 2, 3, 7, 8, 9 } );

    ranges.Insert( 4u );

    REQUIRE( ranges.Size() == 2 );
    CHECK( ranges.Ranges()[0].first == 2u );
    CHECK( ranges.Ranges()[0].last == 4u );
    CHECK( ranges.Ranges()[1].first == 7u );
    CHECK( ranges.Ranges()[1].last == 9u );
}

TEST_CASE( "SequenceRanges joins two runs when a number fills the gap between them", "[sequenceranges]" )
{
    Ranges ranges;
    Insert( ranges, { 1, 2, 4, 5, 10 } );
    REQUIRE( ranges.Size() == 3 );

    ranges.Insert( 3u );

    CHECK( ranges.Size() == 2 );
    CHECK( ranges.RangeSum() == 6 );
    CHECK( Serialized( ranges ) == std::vector<unsigned char>{ 0x00, 0x02, 0x00, 1, 0, 0, 5, 0, 0, 0x01, 10, 0, 0 } );
}

TEST_CASE( "SequenceRanges aligns the count to a byte after a partial byte", "[sequenceranges]" )
{
    Ranges ranges;
    ranges.Insert( 2u );
    BitStream bs;
    bs.Write1();

    ranges.Serialize( &bs, unlimited, false );

    CHECK( Bytes( bs ) == std::vector<unsigned char>{ 0x80, 0x00, 0x01, 0x01, 2, 0, 0 } );
}

TEST_CASE( "SequenceRanges stops at the range that would pass maxBits", "[sequenceranges]" )
{
    // Each range is budgeted as a flag byte plus two 32-bit values, plus the 16-bit count and
    // one bit, against what earlier ranges cost at sizeof( uint24_t ) per value.
    const BitSize_t twoSingles = 16 + 40 + 32 * 2 + 1;

    SECTION( "keeping the ranges" )
    {
        Ranges ranges;
        Insert( ranges, { 1, 3, 5 } );

        CHECK( Serialized( ranges, twoSingles, false ) == std::vector<unsigned char>{ 0x00, 0x02, 0x01, 1, 0, 0, 0x01, 3, 0, 0 } );
        CHECK( ranges.Size() == 3 );
        CHECK( Serialized( ranges ) == std::vector<unsigned char>{ 0x00, 0x03, 0x01, 1, 0, 0, 0x01, 3, 0, 0, 0x01, 5, 0, 0 } );
    }

    SECTION( "clearing what was sent leaves the tail for the next datagram" )
    {
        Ranges ranges;
        Insert( ranges, { 1, 3, 5, 6, 9 } );

        CHECK( Serialized( ranges, twoSingles, true ) == std::vector<unsigned char>{ 0x00, 0x02, 0x01, 1, 0, 0, 0x01, 3, 0, 0 } );
        CHECK( ranges.Size() == 2 );
        CHECK( ranges.RangeSum() == 3 );
        CHECK( Serialized( ranges ) == std::vector<unsigned char>{ 0x00, 0x02, 0x00, 5, 0, 0, 6, 0, 0, 0x01, 9, 0, 0 } );
    }

    SECTION( "a budget too small for one range writes a count of 0 and clears nothing" )
    {
        Ranges ranges;
        Insert( ranges, { 1, 3 } );

        CHECK( Serialized( ranges, 80, true ) == std::vector<unsigned char>{ 0x00, 0x00 } );
        CHECK( ranges.Size() == 2 );
    }

    SECTION( "a run costs a second value against the budget" )
    {
        Ranges ranges;
        Insert( ranges, { 1, 2, 5, 9 } );

        // After the run: 72 bits spent, so the next range needs 72 + 81 = 153.
        CHECK( Serialized( ranges, 152, true ) == std::vector<unsigned char>{ 0x00, 0x01, 0x00, 1, 0, 0, 2, 0, 0 } );
        CHECK( Serialized( ranges, 153, true ) == std::vector<unsigned char>{ 0x00, 0x02, 0x01, 5, 0, 0, 0x01, 9, 0, 0 } );
        CHECK( ranges.Size() == 0 );
    }
}

TEST_CASE( "SequenceRanges deserializes what it serialized", "[sequenceranges]" )
{
    const std::vector<unsigned char> wire{ 0x00, 0x03, 0x01, 1, 0, 0, 0x00, 5, 0, 0, 7, 0, 0, 0x01, 0xFE, 0xFF, 0xFF };
    BitStream bs( Data( wire ), (unsigned)wire.size(), true );
    Ranges ranges;

    REQUIRE( ranges.Deserialize( &bs ) );

    CHECK( ranges.Size() == 3 );
    CHECK( ranges.RangeSum() == 5 );
    CHECK( Serialized( ranges ) == wire );
}

TEST_CASE( "SequenceRanges deserializes after a partial byte by aligning first", "[sequenceranges]" )
{
    const std::vector<unsigned char> wire{ 0x80, 0x00, 0x01, 0x01, 2, 0, 0 };
    BitStream bs( Data( wire ), (unsigned)wire.size(), true );
    bs.IgnoreBits( 1 );
    Ranges ranges;

    REQUIRE( ranges.Deserialize( &bs ) );

    CHECK( Serialized( ranges ) == std::vector<unsigned char>{ 0x00, 0x01, 0x01, 2, 0, 0 } );
}

TEST_CASE( "SequenceRanges deserializes a count of 0 as empty, replacing what it held", "[sequenceranges]" )
{
    const std::vector<unsigned char> wire{ 0x00, 0x00 };
    BitStream bs( Data( wire ), (unsigned)wire.size(), true );
    Ranges ranges;
    Insert( ranges, { 4, 8 } );

    REQUIRE( ranges.Deserialize( &bs ) );

    CHECK( ranges.Size() == 0 );
    CHECK( ranges.RangeSum() == 0 );
}

TEST_CASE( "SequenceRanges rejects a range whose max is below its min", "[sequenceranges]" )
{
    const std::vector<unsigned char> wire{ 0x00, 0x01, 0x00, 5, 0, 0, 3, 0, 0 };
    BitStream bs( Data( wire ), (unsigned)wire.size(), true );
    Ranges ranges;

    CHECK_FALSE( ranges.Deserialize( &bs ) );
}

TEST_CASE( "SequenceRanges rejects a stream shorter than its count", "[sequenceranges]" )
{
    Ranges ranges;

    SECTION( "missing the count" )
    {
        const std::vector<unsigned char> wire{ 0x00 };
        BitStream bs( Data( wire ), (unsigned)wire.size(), true );
        CHECK_FALSE( ranges.Deserialize( &bs ) );
    }

    SECTION( "missing a whole range" )
    {
        const std::vector<unsigned char> wire{ 0x00, 0x02, 0x01, 1, 0, 0 };
        BitStream bs( Data( wire ), (unsigned)wire.size(), true );
        CHECK_FALSE( ranges.Deserialize( &bs ) );
    }

    SECTION( "cut inside a min" )
    {
        const std::vector<unsigned char> wire{ 0x00, 0x01, 0x01, 1, 0 };
        BitStream bs( Data( wire ), (unsigned)wire.size(), true );
        CHECK_FALSE( ranges.Deserialize( &bs ) );
    }

    SECTION( "cut before a max" )
    {
        const std::vector<unsigned char> wire{ 0x00, 0x01, 0x00, 1, 0, 0 };
        BitStream bs( Data( wire ), (unsigned)wire.size(), true );
        CHECK_FALSE( ranges.Deserialize( &bs ) );
    }
}
