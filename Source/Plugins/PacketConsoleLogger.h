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
/// \brief This will write all incoming and outgoing network messages to the log command parser, which can be accessed through Telnet
///

#pragma once

#include "NativeFeatureIncludes.h"
#if _RAKNET_SUPPORT_LogCommandParser == 1 && _RAKNET_SUPPORT_PacketLogger == 1

#include "Plugins/ThreadsafePacketLogger.h"

namespace RakNet {

/// Forward declarations
class LogCommandParser;

/// \ingroup PACKETLOGGER_GROUP
/// \brief Packetlogger that logs to a remote command console
/// \details Lines queue and are written in Update, on the application thread, so they
/// reach the console at the next Receive. ThreadsafePacketLogger's queue cap applies.
class RAK_DLL_EXPORT PacketConsoleLogger : public ThreadsafePacketLogger
{
public:
    PacketConsoleLogger();
    // Writes to the command parser used for logging, which is accessed through a secondary communication layer (such as Telnet or RakNet) - See ConsoleServer.h
    // Call on the application thread.
    virtual void SetLogCommandParser( LogCommandParser* lcp );
    void WriteLog( const char* str ) override;

protected:
    LogCommandParser* logCommandParser;
};

} // namespace RakNet

#endif // _RAKNET_SUPPORT_*
