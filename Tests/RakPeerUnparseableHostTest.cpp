#include "PeerScope.h"
#include "RakNetTypes.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

/*
RakPeer's sends to a host string report a host that doesn't parse as a failure and send
nothing. SendOutOfBand and SendTTL used to send to the UNASSIGNED_SYSTEM_ADDRESS the failed
parse left, and report success. Nothing received can be asserted for that address, so the
return value carries these tests.

Ping used to catch a failed parse by comparing the address to UNASSIGNED_SYSTEM_ADDRESS,
which "0.0.0.0" with port 0 equals too, so it refused that valid address.

The host is numeric with a stray character after it, so the parse fails without a DNS lookup.
*/

using namespace RakNet;

namespace {

const char* const kUnparseableHost = "127.0.0.1!";

// Any port: nothing is sent to it, or nothing listens there.
constexpr unsigned short kAnyPort = 31072;

} // namespace

TEST_CASE( "SendOutOfBand returns false for an unparseable host", "[network]" )
{
    PeerScope peers;
    RakPeerInterface* peer = peers.Client();

    const char data[] = "x";
    CHECK_FALSE( peer->SendOutOfBand( kUnparseableHost, kAnyPort, data, sizeof( data ) ) );
}

TEST_CASE( "SendTTL returns false for an unparseable host", "[network]" )
{
    PeerScope peers;
    RakPeerInterface* peer = peers.Client();

    CHECK_FALSE( peer->SendTTL( kUnparseableHost, kAnyPort, 2 ) );
    CHECK( peer->SendTTL( "127.0.0.1", kAnyPort, 2 ) );
}

TEST_CASE( "Ping returns false for an unparseable host", "[network]" )
{
    PeerScope peers;
    RakPeerInterface* peer = peers.Client();

    CHECK_FALSE( peer->Ping( kUnparseableHost, kAnyPort, false ) );
}

TEST_CASE( "Ping takes 0.0.0.0 with port 0", "[network]" )
{
    PeerScope peers;
    RakPeerInterface* peer = peers.Client();

    CHECK( peer->Ping( "0.0.0.0", 0, false ) );
}
