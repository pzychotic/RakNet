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
#include <exception>
#include <thread>

// std::thread reports construction failure by throwing — std::system_error from the OS, or
// std::bad_alloc from the shared-state allocation — so Create() has to catch to honour its
// return-code contract (see docs/adr/0002-raknet-does-not-use-exceptions.md).
// The same ADR forbids *requiring* exceptions to be enabled, hence the guard: on a -fno-exceptions
// build the throw is a std::terminate the standard library makes unavoidable, and there is nothing
// for this file to catch.
#if defined( __cpp_exceptions ) || defined( __EXCEPTIONS ) || defined( _CPPUNWIND )
#define RAKNET_RAKTHREAD_HAS_EXCEPTIONS 1
#else
#define RAKNET_RAKTHREAD_HAS_EXCEPTIONS 0
#endif

namespace RakNet {

namespace {

/// Applies \a priority to an already-running thread. std::thread offers no way to set scheduling
/// attributes before the thread starts, so the thread may run at its default priority for a short
/// prefix of its life; that race is accepted.
void ApplyThreadPriority( std::thread::native_handle_type hThread, int priority )
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
}

} // namespace

int RakThread::Create( std::function<void( void* )> func, void* arg, int priority )
{
#if RAKNET_RAKTHREAD_HAS_EXCEPTIONS
    try
    {
#endif
        std::thread aThread( func, arg );
        ApplyThreadPriority( aThread.native_handle(), priority );
        aThread.detach();
        return 0;
#if RAKNET_RAKTHREAD_HAS_EXCEPTIONS
    }
    catch( const std::exception& )
    {
        return 1;
    }
#endif
}

} // namespace RakNet
