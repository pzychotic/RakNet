#include "ReliabilityLayerHarness.h"

#include "PacketPriority.h"
#include "RakNetStatistics.h"
#include "ReliabilityLayer.h"

#include <catch2/catch_test_macros.hpp>

/*
Pins how ReliabilityLayer::Send repairs a priority out of range. NUMBER_OF_PRIORITIES is the
count, not a priority, and is repaired to HIGH_PRIORITY like any other. Before, it got through
and indexed the per-priority arrays one past their end. A debug build stops at Send's
RakAssert instead, so the case runs in release builds only.
*/

using namespace RakNet;
using namespace ReliabilityLayerHarness;

TEST_CASE( "ReliabilityLayer::Send repairs a priority of NUMBER_OF_PRIORITIES to HIGH_PRIORITY", "[network]" )
{
#if defined( _DEBUG )
    SKIP( "RakAssert stops a debug build at an out-of-range priority" );
#else
    LayerUnderTest layer;
    char data[] = { 'A' };

    // Two sends, so the second finds the first still buffered.
    for( int i = 0; i < 2; i++ )
    {
        REQUIRE( layer.Layer().Send( data, BYTES_TO_BITS( sizeof( data ) ), (PacketPriority)NUMBER_OF_PRIORITIES, RELIABLE, 0, true, kMTUSize,
                                     RakNet::GetTimeUS(), 0 ) );
    }

    RakNetStatistics statistics;
    layer.Layer().GetStatistics( &statistics );
    CHECK( statistics.messageInSendBuffer[HIGH_PRIORITY] == 2 );
    CHECK( statistics.bytesInSendBuffer[HIGH_PRIORITY] == 2 * sizeof( data ) );
    unsigned int buffered = 0;
    for( int priority = 0; priority < NUMBER_OF_PRIORITIES; priority++ )
        buffered += statistics.messageInSendBuffer[priority];
    CHECK( buffered == 2 );
#endif
}
