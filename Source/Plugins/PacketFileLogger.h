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

namespace RakNet {

/// \ingroup PACKETLOGGER_GROUP
/// \brief Packetlogger that outputs to a file
class RAK_DLL_EXPORT PacketFileLogger : public PacketLogger
{
public:
    PacketFileLogger();
    virtual ~PacketFileLogger();
    void StartLog( const char* filenamePrefix );
    virtual void WriteLog( const char* str );

protected:
    FILE* packetLogFile;
};

} // namespace RakNet

#endif // _RAKNET_SUPPORT_*
