#include "GetTime.h"
#include "RakNetStatistics.h"

#include <catch2/catch_test_macros.hpp>

#include <climits>
#include <cstdint>
#include <string>

/*
StatisticsToString formats a RakNetStatistics report at verbosity 0 (low), 1 (medium) or
2 and above (high). Every field is at its widest printed value, so the expected text is the
longest report each level can produce.
*/

using namespace RakNet;

namespace {

// Connection age the report prints. The half second keeps the printed whole seconds at 5
// unless the test stalls for longer than that between building the statistics and
// formatting them.
constexpr TimeUS kConnectionAgeUS = 5500000;

RakNetStatistics WidestStatistics()
{
    RakNetStatistics s{};
    for( int i = 0; i < RNS_PER_SECOND_METRICS_COUNT; i++ )
    {
        s.valueOverLastSecond[i] = UINT64_MAX;
        s.runningTotal[i] = UINT64_MAX;
    }
    for( int i = 0; i < NUMBER_OF_PRIORITIES; i++ )
    {
        // Printed with %i, so INT_MAX is the widest value that prints as itself.
        s.messageInSendBuffer[i] = INT_MAX;
        s.bytesInSendBuffer[i] = INT_MAX;
    }
    s.messagesInResendBuffer = INT_MAX;
    s.bytesInResendBuffer = UINT64_MAX;
    s.BPSLimitByCongestionControl = UINT64_MAX;
    s.BPSLimitByOutgoingBandwidthLimit = UINT64_MAX;
    s.packetlossLastSecond = 0.25f;
    s.packetlossTotal = 0.5f;
    s.connectionStartTime = GetTimeUS() - kConnectionAgeUS;
    return s;
}

} // namespace

TEST_CASE( "StatisticsToString at verbosity 0 reports the last second's bytes and loss", "[statistics]" )
{
    CHECK( StatisticsToString( WidestStatistics(), 0 ) ==
           "Bytes per second sent     18446744073709551615\n"
           "Bytes per second received 18446744073709551615\n"
           "Current packetloss        25.0%\n" );
}

TEST_CASE( "StatisticsToString at verbosity 1 adds totals, elapsed time and send limits", "[statistics]" )
{
    CHECK( StatisticsToString( WidestStatistics(), 1 ) ==
           "Actual bytes per second sent       18446744073709551615\n"
           "Actual bytes per second received   18446744073709551615\n"
           "Message bytes per second pushed    18446744073709551615\n"
           "Total actual bytes sent            18446744073709551615\n"
           "Total actual bytes received        18446744073709551615\n"
           "Total message bytes pushed         18446744073709551615\n"
           "Current packetloss                 25.0%\n"
           "Average packetloss                 50.0%\n"
           "Elapsed connection time in seconds 5\n"
           "Send capacity                    18446744073709551615 bytes per second (100%)\n"
           "Send limit                       18446744073709551615 (100%)\n" );
}

TEST_CASE( "StatisticsToString at verbosity 2 and above reports every counter and buffer", "[statistics]" )
{
    const std::string expected =
        "Actual bytes per second sent         18446744073709551615\n"
        "Actual bytes per second received     18446744073709551615\n"
        "Message bytes per second sent        18446744073709551615\n"
        "Message bytes per second resent      18446744073709551615\n"
        "Message bytes per second pushed      18446744073709551615\n"
        "Message bytes per second returned    18446744073709551615\n"
        "Message bytes per second ignored     18446744073709551615\n"
        "Total bytes sent                     18446744073709551615\n"
        "Total bytes received                 18446744073709551615\n"
        "Total message bytes sent             18446744073709551615\n"
        "Total message bytes resent           18446744073709551615\n"
        "Total message bytes pushed           18446744073709551615\n"
        "Total message bytes returned         18446744073709551615\n"
        "Total message bytes ignored          18446744073709551615\n"
        "Messages in send buffer, by priority 2147483647,2147483647,2147483647,2147483647\n"
        "Bytes in send buffer, by priority    2147483647,2147483647,2147483647,2147483647\n"
        "Messages in resend buffer            2147483647\n"
        "Bytes in resend buffer               18446744073709551615\n"
        "Current packetloss                   25.0%\n"
        "Average packetloss                   50.0%\n"
        "Elapsed connection time in seconds   5\n"
        "Send capacity                    18446744073709551615 bytes per second (100%)\n"
        "Send limit                       18446744073709551615 (100%)\n";

    CHECK( StatisticsToString( WidestStatistics(), 2 ) == expected );
    CHECK( StatisticsToString( WidestStatistics(), 3 ) == expected );
}

TEST_CASE( "StatisticsToString leaves out a send limit that is zero", "[statistics]" )
{
    RakNetStatistics s = WidestStatistics();
    s.BPSLimitByCongestionControl = 0;
    s.BPSLimitByOutgoingBandwidthLimit = 0;

    const std::string report = StatisticsToString( s, 1 );

    CHECK( report.find( "Send capacity" ) == std::string::npos );
    CHECK( report.find( "Send limit" ) == std::string::npos );
}
