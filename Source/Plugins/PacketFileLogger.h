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
/// \brief This will write all incoming and outgoing network messages to a file
///

#pragma once

#include "NativeFeatureIncludes.h"
#if _RAKNET_SUPPORT_PacketLogger == 1

#include "Plugins/PacketLogger.h"
#include <stdio.h>

#include <mutex>

namespace RakNet {

/// \ingroup PACKETLOGGER_GROUP
/// \brief Packetlogger that outputs to a file
class RAK_DLL_EXPORT PacketFileLogger : public PacketLogger
{
public:
    PacketFileLogger();
    virtual ~PacketFileLogger();
    /// Opens filenamePrefix_<time>.csv, or PacketLog_<time>.csv if filenamePrefix is null,
    /// writes the header to it, and logs to it from then on. Closes the file already open.
    /// \note May be called while the Peer runs.
    void StartLog( const char* filenamePrefix );
    virtual void WriteLog( const char* str );

protected:
    // Guards packetLogFile.
    std::mutex packetLogFileMutex;
    FILE* packetLogFile;
};

} // namespace RakNet

#endif // _RAKNET_SUPPORT_*
