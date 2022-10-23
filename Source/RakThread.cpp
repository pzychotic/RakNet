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
#include "RakNetDefines.h"
#include "RakMemoryOverride.h"

#if defined(_WIN32)
#include "WindowsIncludes.h"
#include <stdio.h>
#include <process.h>
#else
#include <pthread.h>
#endif

namespace RakNet {

#if defined(_WIN32)
int RakThread::Create( unsigned __stdcall start_address( void* ), void *arglist, int priority)
#else
int RakThread::Create( void* start_address( void* ), void *arglist, int priority)
#endif
{
#ifdef _WIN32
	unsigned threadID = 0;
	HANDLE threadHandle = (HANDLE) _beginthreadex( NULL, MAX_ALLOCA_STACK_ALLOCATION*2, start_address, arglist, 0, &threadID );
	
	SetThreadPriority(threadHandle, priority);

	if (threadHandle==0)
	{
		return 1;
	}
	else
	{
		CloseHandle( threadHandle );
		return 0;
	}
#else
	pthread_t threadHandle;
	// Create thread linux
	pthread_attr_t attr;
	sched_param param;
	param.sched_priority=priority;
	pthread_attr_init( &attr );
	pthread_attr_setschedparam(&attr, &param);
	pthread_attr_setstacksize(&attr, MAX_ALLOCA_STACK_ALLOCATION*2);
	pthread_attr_setdetachstate( &attr, PTHREAD_CREATE_DETACHED );
	int res = pthread_create( &threadHandle, &attr, start_address, arglist );
	RakAssert(res==0 && "pthread_create in RakThread.cpp failed.")
	return res;
#endif
}

} // namespace RakNet
