#include "Plugins/StatisticsHistory.h"

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

/*
TimeAndValueQueue::MergeSets on two discrete sets interleaves their values by time. Values
at the same time are both kept, rhs first.
*/

using namespace RakNet;

namespace {

void Fill( StatisticsHistory::TimeAndValueQueue& queue, std::initializer_list<std::pair<Time, SHValueType>> values )
{
    for( const auto& value : values )
        queue.values.push_back( { value.first, value.second } );
}

// "time:value" per value, in queue order.
std::vector<std::string> Values( const StatisticsHistory::TimeAndValueQueue& queue )
{
    std::vector<std::string> values;
    for( const StatisticsHistory::TimeAndValue& value : queue.values )
        values.push_back( std::to_string( value.time ) + ":" + std::to_string( (int)value.val ) );
    return values;
}

} // namespace

TEST_CASE( "MergeSets interleaves two discrete sets by time, rhs first on equal times", "[statisticshistory]" )
{
    // lhs is the longer set, and the two indexes part ways after the first value, so a
    // read of one set at the other set's index shows.
    StatisticsHistory::TimeAndValueQueue lhs;
    Fill( lhs, { { 10, 1 }, { 20, 2 }, { 30, 3 }, { 40, 4 } } );
    StatisticsHistory::TimeAndValueQueue rhs;
    Fill( rhs, { { 5, 100 }, { 20, 200 } } );

    StatisticsHistory::TimeAndValueQueue output;
    StatisticsHistory::TimeAndValueQueue::MergeSets( &lhs, StatisticsHistory::DC_DISCRETE, &rhs, StatisticsHistory::DC_DISCRETE, &output );

    CHECK( Values( output ) == std::vector<std::string>{ "5:100", "10:1", "20:200", "20:2", "30:3", "40:4" } );
}
