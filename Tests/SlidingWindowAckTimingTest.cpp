#include "CCRakNetSlidingWindow.h"
#include "MTUSize.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

/*
Pins when CCRakNetSlidingWindow lets ReliabilityLayer send its pending acks.

Before an RTT is known, acks go out at once. After that, an ack is held for SYN (10 ms)
from the first datagram it acknowledges, and then goes out, whatever the RTT is.

The defect these cases exist for: the unset check converted the double -1 to
CCTimeType, which is undefined behaviour, and clang at -O2 compiled ShouldSendACKs into
a fall-through to GetSenderRTOForACK. Its answer was then the low byte of RTT + SYN, so
a connection whose RTT made that byte zero (20208 and 55536 below) stopped sending acks
until its RTT changed. One that has received ID_DISCONNECTION_NOTIFICATION sends nothing
that would change it, so neither side ever closed.

Tagged [congestion]: nothing here binds a socket or creates a peer.
*/

using namespace RakNet;

namespace {

constexpr CCTimeType kSyn = 10000;
constexpr CCTimeType kFirstArrival = 1000000;

void ReceiveOneDatagram( CCRakNetSlidingWindow& window )
{
    uint32_t skippedMessageCount = 0;
    window.OnGotPacket( 0, false, kFirstArrival, 100, &skippedMessageCount );
}

} // namespace

TEST_CASE( "Acks go out at once while no RTT is known", "[congestion]" )
{
    CCRakNetSlidingWindow window;
    window.Init( 0, MAXIMUM_MTU_SIZE );
    ReceiveOneDatagram( window );

    CHECK( window.ShouldSendACKs( kFirstArrival, 0 ) );
}

TEST_CASE( "Acks are held for SYN after the first unacknowledged datagram, then sent, whatever the RTT", "[congestion]" )
{
    const CCTimeType rtt = GENERATE( as<CCTimeType>{}, 0, 1, 255, 20208, 20209, 30000, 55536 );
    INFO( "RTT " << rtt << " us" );

    CCRakNetSlidingWindow window;
    window.Init( 0, MAXIMUM_MTU_SIZE );
    window.OnAck( kFirstArrival, rtt, false, 0, 0, 0, false, 0 );
    ReceiveOneDatagram( window );

    CHECK_FALSE( window.ShouldSendACKs( kFirstArrival, 0 ) );
    CHECK_FALSE( window.ShouldSendACKs( kFirstArrival + kSyn - 1, 0 ) );
    CHECK( window.ShouldSendACKs( kFirstArrival + kSyn, 0 ) );
    CHECK( window.ShouldSendACKs( kFirstArrival + 10 * kSyn, 0 ) );
}
