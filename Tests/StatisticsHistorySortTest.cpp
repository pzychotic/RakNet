#include "Plugins/StatisticsHistory.h"

#include "GetTime.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

/*
StatisticsHistory keeps its objects in objectId order, which is what the index functions
count in, and GetHistorySorted orders an object's keys by the chosen statistic, with ties
broken by key.
*/

using namespace RakNet;

namespace {

std::vector<uint64_t> ObjectIds( const StatisticsHistory& history )
{
    std::vector<uint64_t> ids;
    for( unsigned int index = 0; index < history.GetObjectCount(); index++ )
        ids.push_back( history.GetObjectAtIndex( index )->objectId );
    return ids;
}

std::vector<std::string> SortedKeys( const StatisticsHistory& history, uint64_t objectId, StatisticsHistory::SHSortOperation sortType )
{
    std::vector<StatisticsHistory::TimeAndValueQueue*> queues;
    REQUIRE( history.GetHistorySorted( objectId, sortType, queues ) );
    std::vector<std::string> keys;
    for( const StatisticsHistory::TimeAndValueQueue* queue : queues )
        keys.push_back( queue->key );
    return keys;
}

} // namespace

TEST_CASE( "StatisticsHistory indexes its objects in objectId order and keeps the others when one is removed", "[statisticshistory]" )
{
    StatisticsHistory history;
    CHECK( history.AddObject( StatisticsHistory::TrackedObjectData( 30, 0, nullptr ) ) );
    CHECK( history.AddObject( StatisticsHistory::TrackedObjectData( 10, 0, nullptr ) ) );
    CHECK( history.AddObject( StatisticsHistory::TrackedObjectData( 20, 0, nullptr ) ) );
    CHECK_FALSE( history.AddObject( StatisticsHistory::TrackedObjectData( 20, 0, nullptr ) ) );

    CHECK( ObjectIds( history ) == std::vector<uint64_t>{ 10, 20, 30 } );
    CHECK( history.GetObjectIndex( 30 ) == 2 );
    CHECK( history.GetObjectIndex( 25 ) == (unsigned int)-1 );

    CHECK( history.RemoveObject( 20, nullptr ) );

    CHECK( ObjectIds( history ) == std::vector<uint64_t>{ 10, 30 } );
    CHECK( history.GetObjectIndex( 30 ) == 1 );
}

TEST_CASE( "StatisticsHistory sorts an object's keys by the statistic, then by key", "[statisticshistory]" )
{
    StatisticsHistory history;
    REQUIRE( history.AddObject( StatisticsHistory::TrackedObjectData( 1, 0, nullptr ) ) );
    const Time now = GetTime();
    history.AddValueByObjectID( 1, "b", 5, now, false );
    history.AddValueByObjectID( 1, "c", 1, now, false );
    history.AddValueByObjectID( 1, "a", 5, now, false );

    CHECK( SortedKeys( history, 1, StatisticsHistory::SH_SORT_BY_RECENT_SUM_ASCENDING ) == std::vector<std::string>{ "c", "a", "b" } );
    CHECK( SortedKeys( history, 1, StatisticsHistory::SH_SORT_BY_RECENT_SUM_DESCENDING ) == std::vector<std::string>{ "b", "a", "c" } );
}
