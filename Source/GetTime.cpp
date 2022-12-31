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

namespace RakNet {

RakNet::Time RakNet::GetTime()
{
    auto now = high_resolution_clock::now();
    return (RakNet::Time)duration_cast<milliseconds>( now.time_since_epoch() ).count();
}

RakNet::TimeMS RakNet::GetTimeMS()
{
    auto now = high_resolution_clock::now();
    return (RakNet::TimeMS)duration_cast<milliseconds>( now.time_since_epoch() ).count();
}

RakNet::TimeUS RakNet::GetTimeUS()
{
    auto now = high_resolution_clock::now();
    return (RakNet::TimeUS)duration_cast<microseconds>( now.time_since_epoch() ).count();
}

bool RakNet::GreaterThan( RakNet::Time a, RakNet::Time b )
{
    // a > b?
    const RakNet::Time halfSpan = ( RakNet::Time )( ( ( RakNet::Time )(const RakNet::Time)-1 ) / (RakNet::Time)2 );
    return b != a && b - a > halfSpan;
}

bool RakNet::LessThan( RakNet::Time a, RakNet::Time b )
{
    // a < b?
    const RakNet::Time halfSpan = ( ( RakNet::Time )(const RakNet::Time)-1 ) / (RakNet::Time)2;
    return b != a && b - a < halfSpan;
}

} // namespace RakNet
