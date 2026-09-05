#include "RakNetTypes.h"
#include "WSAStartupSingleton.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>

/*
Formatting only: no sockets, no peers, nothing that sleeps. SystemAddress::ToString
is pure string assembly over a stored sockaddr, and it is the accessor almost every
log line and every Catch2 failure message in this suite goes through, so it is worth
pinning down on its own rather than only through the network tests.

The regression these cover: ToString( true, dest ) once wrote the port at dest[0],
over the IP it had just written, so "192.168.1.5|1234" came out as "1234". The
buffer sizes below match the contract the header states - dest must hold the
output - measured against the static buffers the const char* overload uses,
22 + 5 + 1.
*/

using namespace RakNet;

namespace {

// The same size ToString( bool, char ) gives its own rotating buffers in the
// IPv4 build: longest dotted quad, delineator, five port digits, terminator.
using AddressBuffer = char[22 + 5 + 1];

// Nothing here opens a socket, but the conversions underneath do: the two-argument
// SystemAddress constructor reaches inet_addr and ToString reaches inet_ntoa, both
// of which Winsock documents as requiring WSAStartup. Every other test in the suite
// gets that for free from RakPeer::Startup; these have no peer, so they take the
// same refcount RakNet itself uses rather than relying on the two functions
// happening to work uninitialised. A no-op off Windows.
struct WinsockFixture
{
    WinsockFixture() { WSAStartupSingleton::AddRef(); }
    ~WinsockFixture() { WSAStartupSingleton::Deref(); }
};

} // namespace

TEST_CASE_METHOD( WinsockFixture, "SystemAddress::ToString appends the port after the IP", "[address]" )
{
    const SystemAddress address( "192.168.1.5", 1234 );

    AddressBuffer dest = {};
    address.ToString( true, dest );

    CHECK( std::strcmp( dest, "192.168.1.5|1234" ) == 0 );
}

TEST_CASE_METHOD( WinsockFixture, "SystemAddress::ToString omits the port when asked to", "[address]" )
{
    const SystemAddress address( "192.168.1.5", 1234 );

    AddressBuffer dest = {};
    address.ToString( false, dest );

    CHECK( std::strcmp( dest, "192.168.1.5" ) == 0 );
}

TEST_CASE_METHOD( WinsockFixture, "SystemAddress::ToString honours the delineator argument", "[address]" )
{
    const SystemAddress address( "192.168.1.5", 1234 );

    AddressBuffer dest = {};
    address.ToString( true, dest, '_' );

    CHECK( std::strcmp( dest, "192.168.1.5_1234" ) == 0 );
}

TEST_CASE_METHOD( WinsockFixture, "SystemAddress::ToString writes the widest port in full", "[address]" )
{
    // 65535 is the longest the port can be, and the case a too-small end pointer
    // would truncate first.
    const SystemAddress address( "255.255.255.255", 65535 );

    AddressBuffer dest = {};
    address.ToString( true, dest );

    CHECK( std::strcmp( dest, "255.255.255.255|65535" ) == 0 );
}

TEST_CASE_METHOD( WinsockFixture, "SystemAddress::ToString names the unassigned address", "[address]" )
{
    AddressBuffer dest = {};
    UNASSIGNED_SYSTEM_ADDRESS.ToString( true, dest );

    CHECK( std::strcmp( dest, "UNASSIGNED_SYSTEM_ADDRESS" ) == 0 );
}

TEST_CASE_METHOD( WinsockFixture, "The static-buffer SystemAddress::ToString agrees with the caller-buffer one", "[address]" )
{
    const SystemAddress address( "192.168.1.5", 1234 );

    // Same code underneath; this is the overload most callers reach for, and the
    // one that carried the bug into RakPeer's logging.
    CHECK( std::strcmp( address.ToString(), "192.168.1.5|1234" ) == 0 );
    CHECK( std::strcmp( address.ToString( false ), "192.168.1.5" ) == 0 );
}

TEST_CASE_METHOD( WinsockFixture, "AddressOrGUID::ToString forwards to the address it holds", "[address]" )
{
    const AddressOrGUID addressOrGuid( SystemAddress( "192.168.1.5", 1234 ) );

    AddressBuffer dest = {};
    addressOrGuid.ToString( true, dest );

    CHECK( std::strcmp( dest, "192.168.1.5|1234" ) == 0 );
}
