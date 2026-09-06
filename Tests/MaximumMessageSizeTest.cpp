#include "PeerScope.h"

#include "BitStream.h"
#include "MTUSize.h"
#include "RakPeerInterface.h"
#include "ReliabilityLayer.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

/*
Pins the send-side message size limit, MAXIMUM_MESSAGE_SIZE (MTUSize.h).

Until this file existed nothing bounded what an application could hand to Send. The
practical limit was "whatever overflows first": RakPeer::Send computed `length * 8` in int,
which is undefined behaviour from length > 268435455, and below that a caller could build a
message far larger than any System would reassemble and learn about it only by having the
far end drop every chunk.

MAXIMUM_MESSAGE_SIZE is the receive-side cap - MAXIMUM_SPLIT_PACKET_COUNT, ReliabilityLayer.h
- times the payload of one datagram at the lowest MTU a connection can negotiate, so the two
ends agree by construction. That derivation is checked by static_asserts in RakPeer.cpp and
ReliabilityLayer.cpp rather than here; what these cases pin is the behaviour at the boundary
and the fact that all three entry points share it.

Deliberately tagged [send] and NOT [network]: a peer is started so Send gets past its
`remoteSystemList == 0` guard, but nothing connects and nothing reaches a socket. Send
buffers the message and the peer is destroyed before any Update sends it.

The accepting case is the expensive one - a real 32 MiB message - and it is not optional:
without it, an off-by-one that rejected *everything* would leave every other assertion here
green.

Rejection is asserted by return value, which is the entire contract: Send documents "0 on
bad input". Every rejecting case passes a one-byte buffer with a large length, which is safe
precisely because the size check lands before `data` is dereferenced. If that ordering is
ever broken these tests crash rather than fail, which is the loudest signal available.
*/

using namespace RakNet;

namespace {

constexpr unsigned short kUnusedPeerPort = 60001;

// What the derivation is expected to produce in a default build: 65536 * ( 576 - 28 - 9 - 23 ).
// Written out rather than recomputed from the expression the header uses, so a change to that
// expression fails here too instead of agreeing with itself.
constexpr unsigned int kExpectedMaximum = 33816576u;

// Payload of one datagram at the lowest negotiable MTU. The other half of the derivation.
constexpr unsigned int kPayloadAtLowestMTU = 516u;

// Enough to be a valid pointer, and never read: every rejecting case is rejected on length
// before Send looks at the buffer.
char g_oneByte[1] = { 0 };

// A started, unconnected peer. Send returns 0 unconditionally until Startup has run, so
// starting one is what makes a rejection assertion mean anything.
RakPeerInterface* StartedPeer( PeerScope& peers )
{
    return peers.Client( kUnusedPeerPort );
}

} // namespace

TEST_CASE( "MAXIMUM_MESSAGE_SIZE is derived from the receive-side cap", "[send]" )
{
    REQUIRE( MAXIMUM_MESSAGE_SIZE == kExpectedMaximum );
    REQUIRE( MAXIMUM_MESSAGE_SIZE == MAXIMUM_SPLIT_PACKET_COUNT * kPayloadAtLowestMTU );

    // The point of the whole exercise: the largest message the send side will emit still
    // fits in the number of chunks the receive side is willing to reassemble.
    REQUIRE( MAXIMUM_MESSAGE_SIZE / kPayloadAtLowestMTU <= MAXIMUM_SPLIT_PACKET_COUNT );
}

TEST_CASE( "Send accepts a message of exactly MAXIMUM_MESSAGE_SIZE", "[send]" )
{
    PeerScope peers;
    RakPeerInterface* peer = StartedPeer( peers );

    // ~32 MiB here plus one copy inside SendBuffered, both released at scope exit.
    std::vector<char> message( MAXIMUM_MESSAGE_SIZE, 'a' );

    const uint32_t receipt = peer->Send( message.data(), (int)message.size(), HIGH_PRIORITY,
                                         RELIABLE_ORDERED, 0, UNASSIGNED_SYSTEM_ADDRESS, true );

    // Non-zero is the documented "accepted" answer.
    REQUIRE( receipt != 0 );
}

TEST_CASE( "Send rejects one byte over MAXIMUM_MESSAGE_SIZE", "[send]" )
{
    PeerScope peers;
    RakPeerInterface* peer = StartedPeer( peers );

    REQUIRE( peer->Send( g_oneByte, (int)( MAXIMUM_MESSAGE_SIZE + 1 ), HIGH_PRIORITY,
                         RELIABLE_ORDERED, 0, UNASSIGNED_SYSTEM_ADDRESS, true ) == 0 );
}

TEST_CASE( "Send rejects lengths that used to overflow length * 8", "[send]" )
{
    PeerScope peers;
    RakPeerInterface* peer = StartedPeer( peers );

    // 2^28, the first length whose bit count was undefined behaviour in int arithmetic.
    REQUIRE( peer->Send( g_oneByte, 268435456, HIGH_PRIORITY, RELIABLE_ORDERED, 0,
                         UNASSIGNED_SYSTEM_ADDRESS, true ) == 0 );

    REQUIRE( peer->Send( g_oneByte, INT32_MAX, HIGH_PRIORITY, RELIABLE_ORDERED, 0,
                         UNASSIGNED_SYSTEM_ADDRESS, true ) == 0 );
}

TEST_CASE( "Send rejects an oversized message addressed to loopback", "[send]" )
{
    PeerScope peers;
    RakPeerInterface* peer = StartedPeer( peers );

    // Loopback never splits and never reaches a socket, so no receive-side cap constrains
    // it. Rejected anyway: Send means the same thing whoever the target is, and a call that
    // succeeds against yourself while failing against everyone else is the kind of asymmetry
    // that gets found in production rather than in a test.
    SystemAddress loopback;
    REQUIRE( loopback.FromStringExplicitPort( "127.0.0.1", peer->GetInternalID().GetPort() ) );

    REQUIRE( peer->Send( g_oneByte, (int)( MAXIMUM_MESSAGE_SIZE + 1 ), HIGH_PRIORITY,
                         RELIABLE_ORDERED, 0, loopback, false ) == 0 );
}

TEST_CASE( "SendList applies the limit to the concatenation", "[send]" )
{
    PeerScope peers;
    RakPeerInterface* peer = StartedPeer( peers );

    // Two blocks, each legal alone, summing to one byte over. The limit is on the message
    // the far end reassembles, which is the concatenation.
    const char* blocks[2] = { g_oneByte, g_oneByte };
    const int half = (int)( MAXIMUM_MESSAGE_SIZE / 2 );
    int lengths[2] = { half, (int)( MAXIMUM_MESSAGE_SIZE - half ) + 1 };

    REQUIRE( peer->SendList( blocks, lengths, 2, HIGH_PRIORITY, RELIABLE_ORDERED, 0,
                             UNASSIGNED_SYSTEM_ADDRESS, true ) == 0 );

    // The case an `unsigned int` total could not see: two lengths summing past 2^32 wrap to
    // a small total, which would pass a check written against the wrapped value and then be
    // used to size an allocation the copy loop overruns. Summed in uint64_t now.
    int wrapping[2] = { INT32_MAX, INT32_MAX };
    REQUIRE( peer->SendList( blocks, wrapping, 2, HIGH_PRIORITY, RELIABLE_ORDERED, 0,
                             UNASSIGNED_SYSTEM_ADDRESS, true ) == 0 );
}

TEST_CASE( "Send( BitStream* ) shares the limit", "[send]" )
{
    PeerScope peers;
    RakPeerInterface* peer = StartedPeer( peers );

    // A BitStream cannot be made oversized without an oversized buffer behind it, so this
    // case pays for one - a single byte over, to pin the same boundary the char* overload has.
    BitStream tooLarge;
    tooLarge.Write( g_oneByte[0] );
    tooLarge.PadWithZeroToByteLength( (unsigned int)( MAXIMUM_MESSAGE_SIZE + 1 ) );
    REQUIRE( tooLarge.GetNumberOfBytesUsed() == MAXIMUM_MESSAGE_SIZE + 1 );

    REQUIRE( peer->Send( &tooLarge, HIGH_PRIORITY, RELIABLE_ORDERED, 0,
                         UNASSIGNED_SYSTEM_ADDRESS, true ) == 0 );
}
