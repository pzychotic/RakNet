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

#include "StringUtils.h"

#include <cinttypes>

namespace RakNet {

ThreadsafePacketLogger::ThreadsafePacketLogger()
: maxQueuedLines( 8192 )
, linesRefused( 0 )
, linesRefusedSinceUpdate( 0 )
{
}
ThreadsafePacketLogger::~ThreadsafePacketLogger()
{
}
void ThreadsafePacketLogger::Update( void )
{
    std::deque<std::string> pending;
    uint64_t refused;
    {
        std::lock_guard<std::mutex> lock( logMessagesMutex );
        pending.swap( logMessages );
        refused = linesRefusedSinceUpdate;
        linesRefusedSinceUpdate = 0;
    }
    // Outside the lock, so a slow WriteLog never holds up the network thread.
    for( const std::string& msg : pending )
        WriteLog( msg.c_str() );
    if( refused != 0 )
        WriteLog( RakNet::format( "%" PRIu64 " log lines refused at the cap", refused ).c_str() );
}
void ThreadsafePacketLogger::SetMaxQueuedLines( unsigned int max )
{
    std::lock_guard<std::mutex> lock( logMessagesMutex );
    maxQueuedLines = max;
}
unsigned int ThreadsafePacketLogger::GetMaxQueuedLines( void ) const
{
    std::lock_guard<std::mutex> lock( logMessagesMutex );
    return maxQueuedLines;
}
uint64_t ThreadsafePacketLogger::GetLinesRefused( void ) const
{
    std::lock_guard<std::mutex> lock( logMessagesMutex );
    return linesRefused;
}
void ThreadsafePacketLogger::AddToLog( const char* str )
{
    std::lock_guard<std::mutex> lock( logMessagesMutex );
    if( logMessages.size() >= maxQueuedLines )
    {
        ++linesRefusedSinceUpdate;
        // Once per logger: a flood would otherwise flood the console too
        if( linesRefused++ == 0 )
            RAKNET_DEBUG_PRINTF( "ThreadsafePacketLogger: refused a log line at SetMaxQueuedLines's %u. See GetLinesRefused.\n", maxQueuedLines );
        return;
    }
    logMessages.emplace_back( str );
}

} // namespace RakNet

#endif // _RAKNET_SUPPORT_*
