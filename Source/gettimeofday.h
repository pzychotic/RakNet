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

#if defined( _WIN32 ) && !defined( __GNUC__ ) && !defined( __GCCXML__ )
#include <time.h>
struct timezone
{
    int tz_minuteswest; /* minutes W of Greenwich */
    int tz_dsttime;     /* type of dst correction */
};

int gettimeofday( struct timeval* tv, struct timezone* tz );

#else

#include <sys/time.h>
#include <unistd.h>

#endif
