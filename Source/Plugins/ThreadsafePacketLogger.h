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
/// \brief Derivation of the packet logger to defer the call to WriteLog until the user thread.
///

#pragma once

#include "NativeFeatureIncludes.h"
#if _RAKNET_SUPPORT_PacketLogger == 1

#include "Plugins/PacketLogger.h"

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

namespace RakNet {

/// \ingroup PACKETLOGGER_GROUP
/// \brief Same as PacketLogger, but writes output in the user thread.
class RAK_DLL_EXPORT ThreadsafePacketLogger : public PacketLogger
{
public:
    ThreadsafePacketLogger();
    virtual ~ThreadsafePacketLogger();

    /// Writes the queued lines. If any were refused since the last call, ends with one
    /// line giving how many.
    virtual void Update( void );

    /// \brief Caps how many lines wait in the queue for Update().
    /// \details At the cap a new line is refused and counted in GetLinesRefused(). Lowering
    /// the cap below the current queue length drops nothing already queued. 0 queues
    /// nothing. Defaults to 8192 lines.
    void SetMaxQueuedLines( unsigned int max );

    /// \return The value passed to SetMaxQueuedLines(), or the default.
    unsigned int GetMaxQueuedLines( void ) const;

    /// \return How many lines SetMaxQueuedLines()'s cap has refused since construction.
    uint64_t GetLinesRefused( void ) const;

protected:
    /// Thread-safe. Called from the network thread and from any thread that logs.
    virtual void AddToLog( const char* str );

private:
    mutable std::mutex logMessagesMutex;
    std::deque<std::string> logMessages;
    unsigned int maxQueuedLines;
    uint64_t linesRefused;
    uint64_t linesRefusedSinceUpdate;
};

} // namespace RakNet

#endif // _RAKNET_SUPPORT_*
