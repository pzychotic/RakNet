#include "Plugins/PacketLogger.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

/*
PacketLogger::UserIDTOString, driven directly. It takes only an id and touches
nothing else on the plugin - no peer, no socket, no waiting - so exercising it is a
function call, which is why this file carries no [network] tag. It is protected and
virtual because the header's own comment says users should override it, so the test
reaches it the way a user would: through a subclass, calling the base implementation
that everyone who does not override it gets.

The regression this covers: the body wrote `res.ptr = '\0'` instead of
`*res.ptr = '\0'`, assigning the past-the-end pointer rather than terminating the
buffer - which MSVC accepts without /permissive- and GCC rejects outright. The
buffer is `static char str[256]`, zero-initialised once and never terminated again,
so any single call in isolation looks correct; it only shows up when a shorter id
follows a longer one and reads the previous call's tail back.
*/

using namespace RakNet;

namespace {

// UserIDTOString is protected, and overriding it is the documented use of this
// class. This one deliberately does not override it - it only widens access to the
// base version under test.
class ExposedPacketLogger : public PacketLogger
{
public:
    using PacketLogger::UserIDTOString;
};

} // namespace

TEST_CASE( "PacketLogger::UserIDTOString terminates its buffer between calls", "[packetlogger]" )
{
    ExposedPacketLogger logger;

    // The widest an unsigned char gets, and the setup for the call that follows.
    // Correct either way, and not only because the buffer starts zeroed: no id
    // renders wider than three digits, so nothing an earlier call anywhere in the
    // process could have left behind reaches past what this one overwrites.
    //
    // Both results are copied out before the next call, because the return points
    // at the static buffer that call reuses.
    const std::string threeDigits( logger.UserIDTOString( 255 ) );
    CHECK( threeDigits == "255" );

    // One digit over the same buffer. Without the terminator str[1..2] still hold
    // '5','5' and this comes back as "755".
    const std::string oneDigit( logger.UserIDTOString( 7 ) );
    CHECK( oneDigit == "7" );
}
