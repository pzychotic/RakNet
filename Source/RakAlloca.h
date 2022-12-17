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

#if defined( __FreeBSD__ )
#include <stdlib.h>
#elif defined( _WIN32 )
#include <malloc.h>
#else
#include <malloc.h>
// Alloca needed on Ubuntu apparently
#include <alloca.h>
#endif
