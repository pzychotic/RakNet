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
#include <mutex>
#include <pthread.h>
#include <sys/types.h>
#endif

#include "Export.h"

namespace RakNet {

class RAK_DLL_EXPORT SignaledEvent
{
public:
    SignaledEvent();
    ~SignaledEvent();

    void InitEvent( void );
    void CloseEvent( void );
    void SetEvent( void );
    void WaitOnEvent( int timeoutMs );

protected:
#ifdef _WIN32
    HANDLE eventList;
#else
    std::mutex isSignaledMutex;
    bool isSignaled;
    pthread_condattr_t condAttr;
    pthread_cond_t eventList;
    pthread_mutex_t hMutex;
    pthread_mutexattr_t mutexAttr;
#endif
};

} // namespace RakNet
