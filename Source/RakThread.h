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

    /// Create a thread, simplified to be cross platform without all the extra junk
    /// To then start that thread, call RakThread::Create(functionName, arguments);
    /// \param[in] func Function you want to call
    /// \param[in] arg Argument to pass to the function
    /// \return 0=success. >0 = error code
    static int Create( std::function<void( void* )> func, void* arg, int priority = 0 );

    // nice value  Win32 Priority
    // -20 to -16  THREAD_PRIORITY_HIGHEST
    // -15 to -6   THREAD_PRIORITY_ABOVE_NORMAL
    // -5  to +4   THREAD_PRIORITY_NORMAL
    // +5  to +14  THREAD_PRIORITY_BELOW_NORMAL
    // +15 to +19  THREAD_PRIORITY_LOWEST
};

} // namespace RakNet
