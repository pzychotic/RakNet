#include "GetTime.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

/*
Guards the clock source behind RakNet::GetTime/GetTimeMS/GetTimeUS.

The defect these cases exist for: the functions were written against
std::chrono::high_resolution_clock, which is only an alias and is not required to
be steady. libstdc++ aliases it to system_clock, the settable wall clock, so an NTP
step or a `date -s` moved the value backwards. Every consumer in the library treats
these values as unsigned deltas, and ReliabilityLayer::AckTimeout underflows on a
backward step of more than ten seconds - dropping every connection on a peer at once.

None of this can be tested by stepping a clock: a unit test may not reset the machine's
wall clock, and the property at stake is which clock was chosen, not a computation. What
IS cheaply observable is the consequence of the choice, and that is what these cases
assert - monotonicity across many samples, and a magnitude that only a process-relative
baseline can produce.

Tagged [time] and deliberately not [network]: nothing here binds a socket or creates a
peer, and `ctest -L time` must stay instant. Nothing here may become [slow].
*/

using namespace RakNet;

namespace {

// A year in microseconds, the ceiling for the baseline case below. Chosen to be
// enormous relative to any test run yet ~53,000x smaller than the microseconds
// elapsed since 1970, so the assertion cannot be confused by how long the suite
// has been running before this file's turn.
const uint64_t oneYearInMicroseconds = 365ull * 24 * 60 * 60 * 1000 * 1000;

} // namespace

// Samples in a tight loop rather than across sleeps: a backward step is what this
// guards against, and consecutive samples are where one would show. 100,000 is
// enough to span several milliseconds of real time without being measurable in the
// suite's runtime.
TEST_CASE( "GetTimeUS never goes backwards", "[time]" )
{
    const int sampleCount = 100000;

    std::vector<TimeUS> samples;
    samples.reserve( sampleCount );
    for( int i = 0; i < sampleCount; i++ )
        samples.push_back( GetTimeUS() );

    for( int i = 1; i < sampleCount; i++ )
        REQUIRE( samples[i] >= samples[i - 1] );

    // A clock that never advanced would pass the check above vacuously.
    REQUIRE( samples[sampleCount - 1] > samples[0] );
}

// GetTime and GetTimeMS return uint32_t in the default build, so a wall-clock
// millisecond count would be truncated rather than obviously huge. Their
// monotonicity is still worth pinning: they are the two the library uses most.
TEST_CASE( "GetTime and GetTimeMS never go backwards", "[time]" )
{
    const int sampleCount = 100000;

    Time previousTime = GetTime();
    TimeMS previousTimeMS = GetTimeMS();
    for( int i = 0; i < sampleCount; i++ )
    {
        const Time currentTime = GetTime();
        const TimeMS currentTimeMS = GetTimeMS();
        REQUIRE( currentTime >= previousTime );
        REQUIRE( currentTimeMS >= previousTimeMS );
        previousTime = currentTime;
        previousTimeMS = currentTimeMS;
    }
}

// The one case that would have caught the original defect directly. On libstdc++ the
// old implementation returned microseconds since 1970 - about 1.8e15, four orders of
// magnitude past this ceiling.
TEST_CASE( "the time baseline is process-relative, not the Unix epoch", "[time]" )
{
    REQUIRE( GetTimeUS() < oneYearInMicroseconds );
}

// One baseline shared by all three, not one per function. If they drifted apart, code
// that mixes them - and the library does - would compute deltas against the wrong zero.
TEST_CASE( "the three time functions share one baseline", "[time]" )
{
    const TimeUS microseconds = GetTimeUS();
    const TimeMS milliseconds = GetTimeMS();

    // milliseconds is sampled second, so it can only be at or ahead of the truncated
    // microsecond reading; a few ms of slack covers a scheduler hiccup between the two.
    const TimeMS truncated = (TimeMS)( microseconds / 1000 );
    REQUIRE( milliseconds >= truncated );
    REQUIRE( milliseconds - truncated < 100 );
}
