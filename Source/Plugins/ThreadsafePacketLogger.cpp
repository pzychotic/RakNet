/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "NativeFeatureIncludes.h"
#if _RAKNET_SUPPORT_PacketLogger == 1

#include "Plugins/ThreadsafePacketLogger.h"

namespace RakNet {

ThreadsafePacketLogger::ThreadsafePacketLogger()
{
}
ThreadsafePacketLogger::~ThreadsafePacketLogger()
{
}
void ThreadsafePacketLogger::Update( void )
{
    std::deque<std::string> pending;
    {
        std::lock_guard<std::mutex> lock( logMessagesMutex );
        pending.swap( logMessages );
    }
    // Outside the lock, so a slow WriteLog never holds up the network thread.
    for( const std::string& msg : pending )
        WriteLog( msg.c_str() );
}
void ThreadsafePacketLogger::AddToLog( const char* str )
{
    std::lock_guard<std::mutex> lock( logMessagesMutex );
    logMessages.emplace_back( str );
}

} // namespace RakNet

#endif // _RAKNET_SUPPORT_*
