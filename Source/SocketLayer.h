/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

/// \file
/// \brief SocketLayer class implementation
///

#pragma once

#include "RakMemoryOverride.h"
#include "RakNetTypes.h"
#include "Export.h"
#include "MTUSize.h"

namespace RakNet {

// A platform independent implementation of Berkeley sockets, with settings used by RakNet
class RAK_DLL_EXPORT SocketLayer
{
public:
    static unsigned short GetLocalPort( __UDPSOCKET__ s );
    static void GetSystemAddress( __UDPSOCKET__ s, SystemAddress* systemAddressOut );

    static void SetSocketOptions( __UDPSOCKET__ listenSocket, bool blockingSocket, bool setBroadcast );
};

} // namespace RakNet
