/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#pragma once

#include "Export.h"

#include <functional>

namespace RakNet {

class RAK_DLL_EXPORT RakThread
{
public:

    /// Create and start a detached thread running func( arg ).
    /// \param[in] func Function you want to call
    /// \param[in] arg Argument to pass to the function
    /// \param[in] priority In force from the thread's first instruction. On Windows a Win32 thread
    /// priority level. On POSIX a sched_priority for the calling thread's policy; one the policy does
    /// not accept, which under SCHED_OTHER is anything but its single value, leaves the thread on the
    /// caller's scheduling.
    /// \return 0=success. >0 = error code: errno from _beginthreadex, or the pthread_create result
    static int Create( std::function<void( void* )> func, void* arg, int priority = 0 );

    // How the Win32 levels correspond to nice values, for a caller choosing one per platform.
    // Create() does not take a nice value: a thread's nice value is inherited from its creator.
    // nice value  Win32 Priority
    // -20 to -16  THREAD_PRIORITY_HIGHEST
    // -15 to -6   THREAD_PRIORITY_ABOVE_NORMAL
    // -5  to +4   THREAD_PRIORITY_NORMAL
    // +5  to +14  THREAD_PRIORITY_BELOW_NORMAL
    // +15 to +19  THREAD_PRIORITY_LOWEST
};

} // namespace RakNet
