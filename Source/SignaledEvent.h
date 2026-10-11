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

#if defined( _WIN32 )
#include "WindowsIncludes.h"
#else
#include <condition_variable>
#include <mutex>
#endif

#include "Export.h"

namespace RakNet {

/// Wakes one waiting thread early.
///
/// The event is a binary latch. Any number of SetEvent calls before a wait make that wait
/// return at once, and the wait clears the latch: signals coalesce, they are not counted.
/// A SetEvent during a wait ends it early. Otherwise the wait returns after about
/// timeoutMs. There is one waiter.
///
/// Windows uses an auto-reset event plus a high-resolution waitable timer. A plain wait
/// there, std::condition_variable::wait_for included, rounds its timeout up to the 15.6 ms
/// system timer tick, and a 1 ms wait would hold an ack for 15 ms. Where Windows has no
/// high-resolution timer, the wait falls back to the plain one.
class RAK_DLL_EXPORT SignaledEvent
{
public:
    SignaledEvent();
    ~SignaledEvent();

    SignaledEvent( const SignaledEvent& ) = delete;
    SignaledEvent& operator=( const SignaledEvent& ) = delete;

    void SetEvent( void );
    void WaitOnEvent( int timeoutMs );

protected:
#ifdef _WIN32
    HANDLE signalEvent;
    /// Ends WaitOnEvent's timeout to the millisecond. Null where Windows has no
    /// high-resolution timer.
    HANDLE waitTimer;
#else
    std::mutex isSignaledMutex;
    std::condition_variable isSignaledCondition;
    bool isSignaled;
#endif
};

} // namespace RakNet
