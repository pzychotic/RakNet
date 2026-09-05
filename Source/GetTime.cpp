/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "GetTime.h"

#include <chrono>

using namespace std::chrono;

namespace {

/// Time elapsed since the first call into any of the three GetTime functions.
///
/// steady_clock explicitly, never high_resolution_clock: the latter is only an alias
/// and the standard does not require it to be steady. libstdc++ aliases it to
/// system_clock - the settable wall clock - where a backward step of more than ten
/// seconds underflows the unsigned deltas RakNet computes from these values and
/// drops every live connection on a peer at once.
///
/// The process-start baseline keeps RakNet::TimeMS (a uint32_t) counting from near
/// zero for a fresh process, so its ~49.7-day wrap is measured from process start
/// rather than from an arbitrary phase. Nothing may assume the baseline is shared
/// between processes: on-wire timestamps already normalize through
/// GetBestClockDifferential precisely because it never was.
///
/// The baseline is a function-local static, not one at namespace scope, so the
/// anchor is the first call rather than this translation unit's dynamic
/// initialization. Deliberate: a namespace-scope static in a library TU has no
/// ordering guarantee against another TU's initializers calling in, and RakNet has
/// no process-start hook to anchor from. First call is the earliest instant the
/// library can name, it is thread-safe under C++11 magic statics, and every caller
/// consumes deltas anyway.
steady_clock::duration ElapsedSinceStart()
{
    static const steady_clock::time_point start = steady_clock::now();
    return steady_clock::now() - start;
}

} // namespace

namespace RakNet {

RakNet::Time RakNet::GetTime()
{
    return (RakNet::Time)duration_cast<milliseconds>( ElapsedSinceStart() ).count();
}

RakNet::TimeMS RakNet::GetTimeMS()
{
    return (RakNet::TimeMS)duration_cast<milliseconds>( ElapsedSinceStart() ).count();
}

RakNet::TimeUS RakNet::GetTimeUS()
{
    return (RakNet::TimeUS)duration_cast<microseconds>( ElapsedSinceStart() ).count();
}

bool RakNet::GreaterThan( RakNet::Time a, RakNet::Time b )
{
    // a > b?
    const RakNet::Time halfSpan = (RakNet::Time)( ( (RakNet::Time)(const RakNet::Time)-1 ) / (RakNet::Time)2 );
    return b != a && b - a > halfSpan;
}

bool RakNet::LessThan( RakNet::Time a, RakNet::Time b )
{
    // a < b?
    const RakNet::Time halfSpan = ( (RakNet::Time)(const RakNet::Time)-1 ) / (RakNet::Time)2;
    return b != a && b - a < halfSpan;
}

} // namespace RakNet
