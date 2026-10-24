#include "EqualsIgnoringAsciiCase.h"

#include <catch2/catch_test_macros.hpp>

#include <string_view>

/*
EqualsIgnoringAsciiCase compares two strings that differ at most in the case of ASCII
letters as equal, and LessIgnoringAsciiCase orders strings the same way. Other bytes compare
as they are.
*/

using namespace RakNet;
using namespace std::string_view_literals;

TEST_CASE( "EqualsIgnoringAsciiCase matches strings that differ only in the case of ASCII letters", "[stringcompare]" )
{
    CHECK( EqualsIgnoringAsciiCase( "help", "HeLP" ) );
    CHECK( EqualsIgnoringAsciiCase( "RakNet 4.0_x", "raknet 4.0_X" ) );
    CHECK( EqualsIgnoringAsciiCase( "", "" ) );
}

TEST_CASE( "EqualsIgnoringAsciiCase tells apart strings that differ in anything else", "[stringcompare]" )
{
    CHECK_FALSE( EqualsIgnoringAsciiCase( "help", "hel" ) );
    CHECK_FALSE( EqualsIgnoringAsciiCase( "hel", "help" ) );
    CHECK_FALSE( EqualsIgnoringAsciiCase( "help", "helq" ) );
    // '@' and '`', and '[' and '{', differ only in the bit that sets a letter's case.
    CHECK_FALSE( EqualsIgnoringAsciiCase( "@[", "`{" ) );
    // Latin-1 'A' with grave and 'a' with grave.
    CHECK_FALSE( EqualsIgnoringAsciiCase( "\xC0", "\xE0" ) );
    // A view's length counts, not a terminator inside it.
    CHECK_FALSE( EqualsIgnoringAsciiCase( "a\0b"sv, "a\0c"sv ) );
}

TEST_CASE( "LessIgnoringAsciiCase orders strings by their bytes with ASCII letters lower-cased", "[stringcompare]" )
{
    CHECK( LessIgnoringAsciiCase( "alpha", "Beta" ) );
    CHECK_FALSE( LessIgnoringAsciiCase( "Beta", "alpha" ) );
    CHECK( LessIgnoringAsciiCase( "hel", "HELP" ) );
    CHECK_FALSE( LessIgnoringAsciiCase( "HELP", "hel" ) );
    // '_' sits between the upper- and lower-case letters, so lower-casing puts it first.
    CHECK( LessIgnoringAsciiCase( "a_b", "AB" ) );
    CHECK_FALSE( LessIgnoringAsciiCase( "AB", "a_b" ) );
    // Latin-1 'A' with grave sorts as it is, after every ASCII byte.
    CHECK( LessIgnoringAsciiCase( "z", "\xC0" ) );
    CHECK( LessIgnoringAsciiCase( "\xC0", "\xE0" ) );
}

TEST_CASE( "LessIgnoringAsciiCase finds neither of two equal strings less", "[stringcompare]" )
{
    const std::string_view pairs[][2] = {
        { "help", "HeLP" },
        { "RakNet 4.0_x", "raknet 4.0_X" },
        { "", "" },
        { "\xC0", "\xC0" },
    };
    for( const auto& pair : pairs )
    {
        CAPTURE( pair[0], pair[1] );
        REQUIRE( EqualsIgnoringAsciiCase( pair[0], pair[1] ) );
        CHECK_FALSE( LessIgnoringAsciiCase( pair[0], pair[1] ) );
        CHECK_FALSE( LessIgnoringAsciiCase( pair[1], pair[0] ) );
    }
}
