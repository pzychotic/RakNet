#include "ReliabilityLayer.h"

#include <catch2/catch_test_macros.hpp>

/*
Pins that BPSTracker holds one entry per distinct time, not one per value.

ReliabilityLayer::Send records every Message with the time of the update cycle that applied
it, so a burst of Sends applied in one cycle shares a single time. One entry per value made
the ClearExpired1 a second later pop the whole burst at once, in the Update that had already
read its clock, and the datagrams that Update sent left timed tens of milliseconds in the
past. Values pushed at one time now add to one entry, so the clear does one pop per time.
*/

using namespace RakNet;

namespace {

// BPSTracker keeps what was pushed in the last second, in CCTimeType units.
constexpr CCTimeType kOneSecond = 1000000;
constexpr CCTimeType kStart = 5 * kOneSecond;

} // namespace

TEST_CASE( "BPSTracker keeps values pushed at the same time in one entry", "[reliability]" )
{
    BPSTracker tracker;
    for( int i = 0; i < 300000; ++i )
    {
        tracker.Push1( kStart, 32 );
    }

    CHECK( tracker.dataQueue.size() == 1 );
    CHECK( tracker.GetBPS1( kStart ) == 300000 * 32 );
    CHECK( tracker.GetTotal1() == 300000 * 32 );
}

TEST_CASE( "BPSTracker drops each time's values once a second has passed since it", "[reliability]" )
{
    BPSTracker tracker;
    tracker.Push1( kStart, 10 );
    tracker.Push1( kStart, 20 );
    tracker.Push1( kStart + kOneSecond / 2, 5 );
    CHECK( tracker.dataQueue.size() == 2 );

    tracker.ClearExpired1( kStart + kOneSecond );
    CHECK( tracker.GetBPS1( kStart + kOneSecond ) == 35 );

    tracker.ClearExpired1( kStart + kOneSecond + 1 );
    CHECK( tracker.GetBPS1( kStart + kOneSecond + 1 ) == 5 );

    tracker.ClearExpired1( kStart + kOneSecond + kOneSecond / 2 + 1 );
    CHECK( tracker.GetBPS1( kStart + kOneSecond + kOneSecond / 2 + 1 ) == 0 );
    CHECK( tracker.dataQueue.empty() );
    CHECK( tracker.GetTotal1() == 35 );
}
