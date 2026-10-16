#include "ParseWholeNumber.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

/*
ParseWholeNumber reads a whole string as a decimal number of the target type, and leaves
the target unchanged for anything else.
*/

using namespace RakNet;

TEST_CASE( "ParseWholeNumber reads a whole decimal number", "[parse]" )
{
    unsigned short port = 1;
    REQUIRE( ParseWholeNumber( "65535", port ) );
    CHECK( port == 65535 );

    uint64_t guid = 1;
    REQUIRE( ParseWholeNumber( "18446744073709551615", guid ) );
    CHECK( guid == UINT64_MAX );

    int length = 1;
    REQUIRE( ParseWholeNumber( "-7", length ) );
    CHECK( length == -7 );
}

TEST_CASE( "ParseWholeNumber leaves the target unchanged for anything but a whole number in range", "[parse]" )
{
    for( const char* text : { "", " 1", "1 ", "+1", "-1", "1x", "0x1", "65536" } )
    {
        CAPTURE( text );
        unsigned short value = 5;
        CHECK_FALSE( ParseWholeNumber( text, value ) );
        CHECK( value == 5 );
    }

    int value = 5;
    CHECK_FALSE( ParseWholeNumber( "2147483648", value ) );
    CHECK( value == 5 );
}
