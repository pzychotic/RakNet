#include "BitStream.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>

/*
BitStream::PrintHex( char* ) writes each byte used as two lowercase hex digits and a
space, and terminates the string.
*/

using namespace RakNet;

TEST_CASE( "BitStream::PrintHex writes each byte as two lowercase hex digits and a space", "[bitstream]" )
{
    BitStream stream;
    const unsigned char bytes[] = { 0x00, 0x0f, 0xa5, 0xff, 0x10 };
    stream.WriteAlignedBytes( bytes, sizeof( bytes ) );

    char out[64];
    std::memset( out, 'x', sizeof( out ) );
    stream.PrintHex( out );

    CHECK( std::strcmp( out, "00 0f a5 ff 10 " ) == 0 );
}
