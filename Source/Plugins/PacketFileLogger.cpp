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
#include "Plugins/PacketFileLogger.h"
#include "GetTime.h"
#include "DiagStall.h" // [DEBUG-stall]

namespace RakNet {

PacketFileLogger::PacketFileLogger()
{
    packetLogFile = 0;
}
PacketFileLogger::~PacketFileLogger()
{
    if( packetLogFile )
    {
        fflush( packetLogFile );
        fclose( packetLogFile );
    }
}
void PacketFileLogger::StartLog( const char* filenamePrefix )
{
    // Open file for writing
    char filename[256];
    if( filenamePrefix )
        snprintf( filename, sizeof( filename ), "%s_%i.csv", filenamePrefix, (int)RakNet::GetTimeMS() );
    else
        snprintf( filename, sizeof( filename ), "PacketLog_%i.csv", (int)RakNet::GetTimeMS() );
    FILE* file = fopen( filename, "wt" );

    // WriteLog takes the lock, so the header is written directly.
    std::lock_guard<std::mutex> lock( packetLogFileMutex );
    if( packetLogFile )
        fclose( packetLogFile );
    packetLogFile = file;
    if( packetLogFile )
    {
        fprintf( packetLogFile, "%s\n", HeaderLine() );
        fflush( packetLogFile );
    }
}

void PacketFileLogger::WriteLog( const char* str )
{
    RAKNET_DIAG_SECTION( diagLock, SEC_FILE_LOCK ); // [DEBUG-stall]
    std::lock_guard<std::mutex> lock( packetLogFileMutex );
    if( packetLogFile )
    {
        {
            RAKNET_DIAG_SECTION( diagPrintf, SEC_FPRINTF ); // [DEBUG-stall]
            fprintf( packetLogFile, "%s\n", str );
        }
        RAKNET_DIAG_SECTION( diagFlush, SEC_FFLUSH ); // [DEBUG-stall]
        fflush( packetLogFile );
    }
}

} // namespace RakNet

#endif // _RAKNET_SUPPORT_*
