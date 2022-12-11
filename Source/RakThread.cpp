/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "RakThread.h"
#include "RakAssert.h"

#if defined( _WIN32 )
#include "WindowsIncludes.h"
#include <processthreadsapi.h>
#else
#include <pthread.h>
#endif
#include <thread>

namespace RakNet {

int RakThread::Create( std::function<void( void* )> func, void* arg, int priority )
{
    std::thread aThread( func, arg );
    std::thread::native_handle_type hThread = aThread.native_handle();

    if( hThread != nullptr )
    {
#ifdef _WIN32
        BOOL res = SetThreadPriority( hThread, priority );
        RakAssert( res != FALSE && "SetThreadPriority in RakThread.cpp failed." );
#else
        int policy;
        sched_param param;
        int resGet = pthread_getschedparam( hThread, &policy, &param );
        RakAssert( resGet == 0 && "pthread_getschedparam in RakThread.cpp failed." );
        param.sched_priority = priority;
        int resSet = pthread_setschedparam( hThread, policy, &param );
        RakAssert( resSet == 0 && "pthread_setschedparam in RakThread.cpp failed." );
#endif

        aThread.detach();
        return 0;
    }

    return 1;
}

} // namespace RakNet
