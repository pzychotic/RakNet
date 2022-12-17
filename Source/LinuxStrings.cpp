/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "LinuxStrings.h"

#if( defined( __GNUC__ ) || defined( __GCCXML__ ) || defined( __S3E__ ) ) && !defined( _WIN32 )

#include <string.h>

#ifndef _stricmp
int _stricmp( const char* s1, const char* s2 )
{
    return strcasecmp( s1, s2 );
}
#endif

int _strnicmp( const char* s1, const char* s2, size_t n )
{
    return strncasecmp( s1, s2, n );
}

#endif
