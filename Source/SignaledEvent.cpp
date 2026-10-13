/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "SignaledEvent.h"

#if defined( _WIN32 )
#include "RakMemoryOverride.h"

#include <cstdlib>
#else
#include <chrono>
#endif

namespace RakNet {

SignaledEvent::SignaledEvent()
{
#ifdef _WIN32
    signalEvent = CreateEvent( 0, false, false, 0 );
    if( signalEvent == NULL )
    {
        // Kernel object exhaustion is fatal, like allocation failure (ADR-0004).
        notifyOutOfMemory( __FILE__, __LINE__ );
        std::abort();
    }
    waitTimer = CreateWaitableTimerExW( NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS );
#else
    isSignaled = false;
#endif
}

SignaledEvent::~SignaledEvent()
{
#ifdef _WIN32
    CloseHandle( signalEvent );
    if( waitTimer != NULL )
        CloseHandle( waitTimer );
#endif
}

void SignaledEvent::SetEvent( void )
{
#ifdef _WIN32
    ::SetEvent( signalEvent );
#else
    {
        std::lock_guard<std::mutex> lock( isSignaledMutex );
        isSignaled = true;
    }
    isSignaledCondition.notify_one();
#endif
}

void SignaledEvent::WaitOnEvent( int timeoutMs )
{
#ifdef _WIN32
    LARGE_INTEGER dueTime;
    dueTime.QuadPart = -(LONGLONG)timeoutMs * 10000; // Relative, in 100 ns units
    if( waitTimer != NULL && timeoutMs >= 0 && SetWaitableTimer( waitTimer, &dueTime, 0, NULL, NULL, FALSE ) )
    {
        HANDLE handles[2] = { signalEvent, waitTimer };
        // The timer is the only timeout: a wait's own timeout runs on the timer tick and can end early.
        WaitForMultipleObjectsEx( 2, handles, FALSE, INFINITE, FALSE );
    }
    else
    {
        WaitForSingleObjectEx( signalEvent, timeoutMs, FALSE );
    }
#else
    std::unique_lock<std::mutex> lock( isSignaledMutex );
    isSignaledCondition.wait_for( lock, std::chrono::milliseconds( timeoutMs ), [this] { return isSignaled; } );
    isSignaled = false;
#endif
}

} // namespace RakNet
