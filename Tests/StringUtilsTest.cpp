#include "StringUtils.h"

#include <catch2/catch_test_macros.hpp>

#include <cinttypes>
#include <cstdint>
#include <string>

/*
RakNet::format is the printf front end the plugins build their debug and log lines
with. It is pure string assembly: no sockets, no peers, nothing that sleeps.

The cases pin the two things a caller relies on: 64-bit values print in full through
the <cinttypes> macros, and a format vsnprintf rejects yields an empty string.
*/

using namespace RakNet;

TEST_CASE( "format prints a 64-bit value in full", "[format]" )
{
    const uint64_t value = 0x123456789ABCDEF0ull;
    const int64_t negative = -0x123456789ABCDEFll;

    CHECK( format( "%" PRIu64, value ) == "1311768467463790320" );
    CHECK( format( "%" PRId64, negative ) == "-81985529216486895" );
}

TEST_CASE( "format returns the whole output however long it is", "[format]" )
{
    const std::string long_text( 4096, 'x' );

    CHECK( format( "<%s>", long_text.c_str() ) == "<" + long_text + ">" );
    CHECK( format( "plain" ) == "plain" );
}

TEST_CASE( "format returns an empty string when vsnprintf fails", "[format]" )
{
    // The "C" locale has no multibyte encoding for this character, so the
    // conversion fails and vsnprintf returns a negative value.
    const wchar_t unencodable[] = L"\x100";

    CHECK( format( "%ls", unencodable ).empty() );
}
