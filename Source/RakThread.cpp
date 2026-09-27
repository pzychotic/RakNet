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
#include <cerrno>
#include <process.h>
#else
#include <pthread.h>
#include <sched.h>
#endif
#include <memory>
#include <utility>

// Threads are created natively, not through std::thread: its constructor reports failure only
// by throwing, which a build without exceptions turns into std::terminate. Both native calls
// report failure by return value, so Create() fails the same way in both modes (see
// ADR-0004). They also take the priority at creation.

namespace RakNet {

namespace {

/// What the new thread needs to call the user's function. Create() keeps std::function, so
/// callers may still pass a capturing lambda; the price is this one heap allocation, freed by the
/// new thread or, if creation fails, by Create(). Allocation failure is fatal under ADR-0004.
struct ThreadStart
{
    std::function<void( void* )> func;
    void* arg;
};

/// Frees the ThreadStart before running the function, which for the library's own threads lasts
/// until shutdown.
void RunThreadStart( void* startPtr )
{
    std::unique_ptr<ThreadStart> start( static_cast<ThreadStart*>( startPtr ) );
    std::function<void( void* )> func = std::move( start->func );
    void* arg = start->arg;
    start.reset();
    func( arg );
}

#if defined( _WIN32 )

unsigned __stdcall ThreadEntry( void* startPtr )
{
    RunThreadStart( startPtr );
    return 0;
}

#else

void* ThreadEntry( void* startPtr )
{
    RunThreadStart( startPtr );
    return nullptr;
}

/// Makes \a attr create a detached thread on the calling thread's policy, at \a priority if that
/// policy accepts it and it differs from the caller's own. RakPeer and TCPInterface default to 1000,
/// which no policy accepts; such a priority leaves the new thread inheriting the creator's
/// scheduling, as it always has on POSIX. A priority the policy accepts but the process may not
/// take (above RLIMIT_RTPRIO, say) makes pthread_create fail, and Create() reports that.
/// \return 0, or the error that leaves \a attr unable to create a detached thread
int InitThreadAttributes( pthread_attr_t& attr, int priority )
{
    int res = pthread_attr_setdetachstate( &attr, PTHREAD_CREATE_DETACHED );
    if( res != 0 )
        return res;

    int policy;
    sched_param param;
    if( pthread_getschedparam( pthread_self(), &policy, &param ) != 0 )
        return 0;
    if( priority == param.sched_priority || priority < sched_get_priority_min( policy ) || priority > sched_get_priority_max( policy ) )
        return 0;

    param.sched_priority = priority;
    if( pthread_attr_setinheritsched( &attr, PTHREAD_EXPLICIT_SCHED ) == 0 && pthread_attr_setschedpolicy( &attr, policy ) == 0 && pthread_attr_setschedparam( &attr, &param ) == 0 )
        return 0;

    // Leave a half-applied explicit schedule behind and pthread_create would use it.
    pthread_attr_setinheritsched( &attr, PTHREAD_INHERIT_SCHED );
    return 0;
}

#endif

} // namespace

int RakThread::Create( std::function<void( void* )> func, void* arg, int priority )
{
    std::unique_ptr<ThreadStart> start( new ThreadStart{ std::move( func ), arg } );

#if defined( _WIN32 )
    // Created suspended so the priority is in force before the thread's first instruction.
    unsigned threadId;
    HANDLE hThread = reinterpret_cast<HANDLE>( _beginthreadex( nullptr, 0, ThreadEntry, start.get(), CREATE_SUSPENDED, &threadId ) );
    if( hThread == nullptr )
    {
        int err = errno;
        return err != 0 ? err : 1;
    }
    start.release();

    BOOL resPriority = SetThreadPriority( hThread, priority );
    RakAssert( resPriority != FALSE && "SetThreadPriority in RakThread.cpp failed." );
    (void)resPriority;

    // Cannot fail: the handle is ours, fresh, and carries THREAD_SUSPEND_RESUME.
    DWORD resResume = ResumeThread( hThread );
    RakAssert( resResume != static_cast<DWORD>( -1 ) && "ResumeThread in RakThread.cpp failed." );
    (void)resResume;

    // Detaches: the thread runs on, and nothing here waits for it.
    CloseHandle( hThread );
    return 0;
#else
    pthread_attr_t attr;
    int res = pthread_attr_init( &attr );
    if( res != 0 )
        return res;
    res = InitThreadAttributes( attr, priority );
    if( res != 0 )
    {
        pthread_attr_destroy( &attr );
        return res;
    }

    pthread_t thread;
    res = pthread_create( &thread, &attr, ThreadEntry, start.get() );
    pthread_attr_destroy( &attr );
    if( res != 0 )
        return res;
    start.release();
    return 0;
#endif
}

} // namespace RakNet
