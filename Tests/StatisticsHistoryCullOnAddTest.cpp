#include "Plugins/StatisticsHistory.h"

#include <catch2/catch_test_macros.hpp>

/*
StatisticsHistory keeps only the values inside timeToTrackValues, even when nothing reads them.

AddValueByIndex appended to the key's queue and never culled it; only the read paths did. A
Peer that recorded statistics but seldom read them held every sample forever, and recentSum
and recentSumOfSquares still counted expired values until the next read.
*/

using namespace RakNet;

namespace {

const uint64_t kObjectId = 7;
const char* const kKey = "bytes";
const Time kWindow = 100;

// Looks at a key's queue as held, without the cull every public read path does first.
class PeekingStatisticsHistory : public StatisticsHistory
{
public:
    const TimeAndValueQueue& HeldQueue( void ) const
    {
        return *objects[GetObjectIndex( kObjectId )]->dataQueues.at( kKey );
    }
};

} // namespace

TEST_CASE( "StatisticsHistory culls expired values on add, with no reads", "[StatisticsHistory]" )
{
    PeekingStatisticsHistory history;
    history.SetDefaultTimeToTrack( kWindow );
    REQUIRE( history.AddObject( StatisticsHistory::TrackedObjectData( kObjectId, 0, nullptr ) ) );

    // One value per 10 ms across five windows, each value distinct so the sums pin which are held.
    const Time kStep = 10;
    const Time kStart = 1000;
    for( Time t = kStart; t <= kStart + 5 * kWindow; t += kStep )
    {
        const SHValueType val = static_cast<SHValueType>( t - kStart + 1 );
        REQUIRE( history.AddValueByObjectID( kObjectId, kKey, val, t, false ) );

        const StatisticsHistory::TimeAndValueQueue& queue = history.HeldQueue();

        CHECK( queue.values.size() <= kWindow / kStep + 1 );

        SHValueType sum = 0;
        SHValueType sumOfSquares = 0;
        for( const StatisticsHistory::TimeAndValue& tav : queue.values )
        {
            CHECK( t - tav.time <= kWindow );
            sum += tav.val;
            sumOfSquares += tav.val * tav.val;
        }
        CHECK( queue.GetRecentSum() == sum );
        CHECK( queue.GetRecentSumOfSquares() == sumOfSquares );
    }

    CHECK( history.HeldQueue().values.size() == kWindow / kStep + 1 );
}

TEST_CASE( "StatisticsHistory keeps newer values when an add is older than them", "[StatisticsHistory]" )
{
    PeekingStatisticsHistory history;
    history.SetDefaultTimeToTrack( kWindow );
    REQUIRE( history.AddObject( StatisticsHistory::TrackedObjectData( kObjectId, 0, nullptr ) ) );

    REQUIRE( history.AddValueByObjectID( kObjectId, kKey, 1, 2000, false ) );
    REQUIRE( history.AddValueByObjectID( kObjectId, kKey, 2, 2050, false ) );
    // Time is unsigned: an age taken from this older time wraps, and must not read as expired.
    REQUIRE( history.AddValueByObjectID( kObjectId, kKey, 4, 1990, false ) );

    const StatisticsHistory::TimeAndValueQueue& queue = history.HeldQueue();
    CHECK( queue.values.size() == 3 );
    CHECK( queue.GetRecentSum() == 7 );
}
