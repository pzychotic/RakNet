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

namespace RakNet {

class WSAStartupSingleton
{
public:
    WSAStartupSingleton();
    ~WSAStartupSingleton();
    static void AddRef( void );
    static void Deref( void );

protected:
    /// Guarded by a mutex private to WSAStartupSingleton.cpp; AddRef and Deref may be
    /// called from any thread.
    static int refCount;
};

} // namespace RakNet
