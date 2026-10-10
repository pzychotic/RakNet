#include "CCRakNetSlidingWindow.h"
#include "MTUSize.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

/*
Pins when CCRakNetSlidingWindow lets ReliabilityLayer send its pending acks.

The first ack after a quiet spell goes out in the update cycle that handled its datagram.
After an ack datagram goes out, the next waits one millisecond, so a stream of datagrams is
acknowledged about once a millisecond rather than once per update cycle. Neither depends on
the RTT.

The RTT values below are the ones an earlier defect stopped acks for. The unset check
converted the double -1 to CCTimeType, which is undefined behaviour, and clang at -O2
compiled ShouldSendACKs into a fall-through whose answer was the low byte of RTT + 10 ms,
so a connection whose RTT made that byte zero (20208 and 55536 below) stopped sending acks
until its RTT changed.

Tagged [congestion]: nothing here binds a socket or creates a peer.
*/

using namespace RakNet;

namespace {

constexpr CCTimeType kAckGap = 1000;
constexpr CCTimeType kFirstArrival = 1000000;

void ReceiveOneDatagram( CCRakNetSlidingWindow& window, CCTimeType when )
{
    uint32_t skippedMessageCount = 0;
    window.OnGotPacket( 0, false, when, 100, &skippedMessageCount );
}

CCTimeType GenerateRtt()
{
    return GENERATE( as<CCTimeType>{}, 0, 1, 255, 20208, 20209, 30000, 55536 );
}

} // namespace

TEST_CASE( "The first ack goes out at once while no RTT is known", "[congestion]" )
{
    CCRakNetSlidingWindow window;
    window.Init( 0, MAXIMUM_MTU_SIZE );
    ReceiveOneDatagram( window, kFirstArrival );

    CHECK( window.ShouldSendACKs( kFirstArrival, 0 ) );
}

TEST_CASE( "The first ack after a quiet spell goes out at once, whatever the RTT", "[congestion]" )
{
    const CCTimeType rtt = GenerateRtt();
    INFO( "RTT " << rtt << " us" );

    CCRakNetSlidingWindow window;
    window.Init( 0, MAXIMUM_MTU_SIZE );
    window.OnAck( kFirstArrival, rtt, false, 0, 0, 0, false, 0 );
    window.OnSendAck( kFirstArrival, 16 );
    ReceiveOneDatagram( window, kFirstArrival + 10 * kAckGap );

    CHECK( window.ShouldSendACKs( kFirstArrival + 10 * kAckGap, 0 ) );
}

TEST_CASE( "After an ack goes out the next waits a millisecond, whatever the RTT", "[congestion]" )
{
    const CCTimeType rtt = GenerateRtt();
    INFO( "RTT " << rtt << " us" );

    CCRakNetSlidingWindow window;
    window.Init( 0, MAXIMUM_MTU_SIZE );
    window.OnAck( kFirstArrival, rtt, false, 0, 0, 0, false, 0 );
    window.OnSendAck( kFirstArrival, 16 );
    ReceiveOneDatagram( window, kFirstArrival + 10 );

    CHECK_FALSE( window.ShouldSendACKs( kFirstArrival + 10, 0 ) );
    CHECK_FALSE( window.ShouldSendACKs( kFirstArrival + kAckGap - 1, 0 ) );
    CHECK( window.ShouldSendACKs( kFirstArrival + kAckGap, 0 ) );
}
