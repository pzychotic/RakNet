#include "EqualsIgnoringAsciiCase.h"

#include <catch2/catch_test_macros.hpp>

#include <string_view>

/*
EqualsIgnoringAsciiCase compares two strings that differ at most in the case of ASCII
letters as equal. Other bytes compare as they are.
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
