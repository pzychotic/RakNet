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

/// Internal

#if defined( _WIN32 )
#define closesocket__ closesocket
#else
#define closesocket__ close
#endif
#define accept__ accept
#define connect__ connect
#define socket__ socket
#define bind__ bind
#define getsockname__ getsockname
#define getsockopt__ getsockopt
#define inet_addr__ inet_addr
#define ioctlsocket__ ioctlsocket
#define listen__ listen
#define recv__ recv
#define recvfrom__ recvfrom
#define select__ select
#define sendto__ sendto
#define send__ send
#define setsockopt__ setsockopt
#define shutdown__ shutdown
