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

#if defined( __GNUC__ )
#include <sys/time.h>
#include <unistd.h>
#endif

namespace RakNet {

SignaledEvent::SignaledEvent()
{
#ifdef _WIN32
    eventList = INVALID_HANDLE_VALUE;
#else
    isSignaled = false;
#endif
}
SignaledEvent::~SignaledEvent()
{
    // Intentionally do not close event, so it doesn't close twice on linux
}

void SignaledEvent::InitEvent( void )
{
#if defined( _WIN32 )
    eventList = CreateEvent( 0, false, false, 0 );
#else
    pthread_condattr_init( &condAttr );
    pthread_cond_init( &eventList, &condAttr );
    pthread_mutexattr_init( &mutexAttr );
    pthread_mutex_init( &hMutex, &mutexAttr );
#endif
}

void SignaledEvent::CloseEvent( void )
{
#ifdef _WIN32
    if( eventList != INVALID_HANDLE_VALUE )
    {
        CloseHandle( eventList );
        eventList = INVALID_HANDLE_VALUE;
    }
#else
    pthread_cond_destroy( &eventList );
    pthread_mutex_destroy( &hMutex );
    pthread_condattr_destroy( &condAttr );
    pthread_mutexattr_destroy( &mutexAttr );
#endif
}

void SignaledEvent::SetEvent( void )
{
#ifdef _WIN32
    ::SetEvent( eventList );
#else
    // Different from SetEvent which stays signaled.
    // We have to record manually that the event was signaled
    isSignaledMutex.lock();
    isSignaled = true;
    isSignaledMutex.unlock();

    // Unblock waiting threads
    pthread_cond_broadcast( &eventList );
#endif
}

void SignaledEvent::WaitOnEvent( int timeoutMs )
{
#ifdef _WIN32
    WaitForSingleObjectEx( eventList, timeoutMs, FALSE );
#else

    // If was previously set signaled, just unset and return
    isSignaledMutex.lock();
    if( isSignaled == true )
    {
        isSignaled = false;
        isSignaledMutex.unlock();
        return;
    }
    isSignaledMutex.unlock();

    // Else wait for SetEvent to be called
    struct timespec ts;
    struct timeval tp;
    int rc = gettimeofday( &tp, NULL );
    ts.tv_sec = tp.tv_sec;
    ts.tv_nsec = tp.tv_usec * 1000;

    while( timeoutMs > 30 )
    {
        // Wait 30 milliseconds for the signal, then check again.
        // This is in case we  missed the signal between the top of this function and pthread_cond_timedwait, or after the end of the loop and pthread_cond_timedwait
        ts.tv_nsec += 30 * 1000000;
        if( ts.tv_nsec >= 1000000000 )
        {
            ts.tv_nsec -= 1000000000;
            ts.tv_sec++;
        }

        // [SBC] added mutex lock/unlock around cond_timedwait.
        // this prevents airplay from generating a whole much of errors.
        // not sure how this works on other platforms since according to
        // the docs you are suppost to hold the lock before you wait
        // on the cond.
        pthread_mutex_lock( &hMutex );
        pthread_cond_timedwait( &eventList, &hMutex, &ts );
        pthread_mutex_unlock( &hMutex );

        timeoutMs -= 30;

        isSignaledMutex.lock();
        if( isSignaled == true )
        {
            isSignaled = false;
            isSignaledMutex.unlock();
            return;
        }
        isSignaledMutex.unlock();
    }

    // Wait the remaining time, and turn off the signal in case it was set
    ts.tv_nsec += timeoutMs * 1000000;
    if( ts.tv_nsec >= 1000000000 )
    {
        ts.tv_nsec -= 1000000000;
        ts.tv_sec++;
    }

    pthread_mutex_lock( &hMutex );
    pthread_cond_timedwait( &eventList, &hMutex, &ts );
    pthread_mutex_unlock( &hMutex );

    isSignaledMutex.lock();
    isSignaled = false;
    isSignaledMutex.unlock();

#endif
}

} // namespace RakNet
