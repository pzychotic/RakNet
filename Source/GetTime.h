/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

/// \file GetTime.h
/// \brief Returns the value from QueryPerformanceCounter.  This is the function RakNet uses to represent time. This time won't match the time returned by GetTimeCount(). See http://www.jenkinssoftware.com/forum/index.php?topic=2798.0
///

#pragma once

#include "Export.h"
#include "RakNetTime.h" // For RakNet::TimeMS

namespace RakNet {

/// Same as GetTimeMS
/// Holds the time in either a 32 or 64 bit variable, depending on __GET_TIME_64BIT
RakNet::Time RAK_DLL_EXPORT GetTime();

/// Return the time as 32 bit
/// \note Counted from a per-process baseline, not a shared epoch. Only deltas are meaningful.
RakNet::TimeMS RAK_DLL_EXPORT GetTimeMS();

/// Return the time as 64 bit
/// \note Counted from a per-process baseline, not a shared epoch. Only deltas are meaningful.
RakNet::TimeUS RAK_DLL_EXPORT GetTimeUS();

/// a > b?
extern RAK_DLL_EXPORT bool GreaterThan( RakNet::Time a, RakNet::Time b );
/// a < b?
extern RAK_DLL_EXPORT bool LessThan( RakNet::Time a, RakNet::Time b );

} // namespace RakNet
