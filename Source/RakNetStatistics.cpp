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
///


#include "RakNetStatistics.h"
#include <cinttypes>
#include <stdio.h> // sprintf
#include <string.h> // strcat
#include "GetTime.h"

namespace RakNet {

// Verbosity level currently supports 0 (low), 1 (medium), 2 (high)
// Buffer must be hold enough to hold the output string.  See the source to get an idea of how many bytes will be output
void RAK_DLL_EXPORT StatisticsToString( RakNetStatistics* s, char* buffer, int verbosityLevel )
{
    if( s == 0 )
    {
        sprintf( buffer, "stats is a NULL pointer in statsToString\n" );
        return;
    }

    if( verbosityLevel == 0 )
    {
        sprintf( buffer,
                 "Bytes per second sent     %" PRIu64 "\n"
                 "Bytes per second received %" PRIu64 "\n"
                 "Current packetloss        %.1f%%\n",
                 s->valueOverLastSecond[ACTUAL_BYTES_SENT],
                 s->valueOverLastSecond[ACTUAL_BYTES_RECEIVED],
                 s->packetlossLastSecond * 100.0f );
    }
    else if( verbosityLevel == 1 )
    {
        sprintf( buffer,
                 "Actual bytes per second sent       %" PRIu64 "\n"
                 "Actual bytes per second received   %" PRIu64 "\n"
                 "Message bytes per second pushed    %" PRIu64 "\n"
                 "Total actual bytes sent            %" PRIu64 "\n"
                 "Total actual bytes received        %" PRIu64 "\n"
                 "Total message bytes pushed         %" PRIu64 "\n"
                 "Current packetloss                 %.1f%%\n"
                 "Average packetloss                 %.1f%%\n"
                 "Elapsed connection time in seconds %" PRIu64 "\n",
                 s->valueOverLastSecond[ACTUAL_BYTES_SENT],
                 s->valueOverLastSecond[ACTUAL_BYTES_RECEIVED],
                 s->valueOverLastSecond[USER_MESSAGE_BYTES_PUSHED],
                 s->runningTotal[ACTUAL_BYTES_SENT],
                 s->runningTotal[ACTUAL_BYTES_RECEIVED],
                 s->runningTotal[USER_MESSAGE_BYTES_PUSHED],
                 s->packetlossLastSecond * 100.0f,
                 s->packetlossTotal * 100.0f,
                 ( RakNet::GetTimeUS() - s->connectionStartTime ) / 1000000 );

        if( s->BPSLimitByCongestionControl != 0 )
        {
            char buff2[128];
            sprintf( buff2,
                     "Send capacity                    %" PRIu64 " bytes per second (%.0f%%)\n",
                     s->BPSLimitByCongestionControl,
                     100.0f * s->valueOverLastSecond[ACTUAL_BYTES_SENT] / s->BPSLimitByCongestionControl );
            strcat( buffer, buff2 );
        }
        if( s->BPSLimitByOutgoingBandwidthLimit != 0 )
        {
            char buff2[128];
            sprintf( buff2,
                     "Send limit                       %" PRIu64 " (%.0f%%)\n",
                     s->BPSLimitByOutgoingBandwidthLimit,
                     100.0f * s->valueOverLastSecond[ACTUAL_BYTES_SENT] / s->BPSLimitByOutgoingBandwidthLimit );
            strcat( buffer, buff2 );
        }
    }
    else
    {
        sprintf( buffer,
                 "Actual bytes per second sent         %" PRIu64 "\n"
                 "Actual bytes per second received     %" PRIu64 "\n"
                 "Message bytes per second sent        %" PRIu64 "\n"
                 "Message bytes per second resent      %" PRIu64 "\n"
                 "Message bytes per second pushed      %" PRIu64 "\n"
                 "Message bytes per second returned    %" PRIu64 "\n"
                 "Message bytes per second ignored     %" PRIu64 "\n"
                 "Total bytes sent                     %" PRIu64 "\n"
                 "Total bytes received                 %" PRIu64 "\n"
                 "Total message bytes sent             %" PRIu64 "\n"
                 "Total message bytes resent           %" PRIu64 "\n"
                 "Total message bytes pushed           %" PRIu64 "\n"
                 "Total message bytes returned         %" PRIu64 "\n"
                 "Total message bytes ignored          %" PRIu64 "\n"
                 "Messages in send buffer, by priority %i,%i,%i,%i\n"
                 "Bytes in send buffer, by priority    %i,%i,%i,%i\n"
                 "Messages in resend buffer            %i\n"
                 "Bytes in resend buffer               %" PRIu64 "\n"
                 "Current packetloss                   %.1f%%\n"
                 "Average packetloss                   %.1f%%\n"
                 "Elapsed connection time in seconds   %" PRIu64 "\n",
                 s->valueOverLastSecond[ACTUAL_BYTES_SENT],
                 s->valueOverLastSecond[ACTUAL_BYTES_RECEIVED],
                 s->valueOverLastSecond[USER_MESSAGE_BYTES_SENT],
                 s->valueOverLastSecond[USER_MESSAGE_BYTES_RESENT],
                 s->valueOverLastSecond[USER_MESSAGE_BYTES_PUSHED],
                 s->valueOverLastSecond[USER_MESSAGE_BYTES_RECEIVED_PROCESSED],
                 s->valueOverLastSecond[USER_MESSAGE_BYTES_RECEIVED_IGNORED],
                 s->runningTotal[ACTUAL_BYTES_SENT],
                 s->runningTotal[ACTUAL_BYTES_RECEIVED],
                 s->runningTotal[USER_MESSAGE_BYTES_SENT],
                 s->runningTotal[USER_MESSAGE_BYTES_RESENT],
                 s->runningTotal[USER_MESSAGE_BYTES_PUSHED],
                 s->runningTotal[USER_MESSAGE_BYTES_RECEIVED_PROCESSED],
                 s->runningTotal[USER_MESSAGE_BYTES_RECEIVED_IGNORED],
                 s->messageInSendBuffer[IMMEDIATE_PRIORITY], s->messageInSendBuffer[HIGH_PRIORITY], s->messageInSendBuffer[MEDIUM_PRIORITY], s->messageInSendBuffer[LOW_PRIORITY],
                 (unsigned int)s->bytesInSendBuffer[IMMEDIATE_PRIORITY], (unsigned int)s->bytesInSendBuffer[HIGH_PRIORITY], (unsigned int)s->bytesInSendBuffer[MEDIUM_PRIORITY], (unsigned int)s->bytesInSendBuffer[LOW_PRIORITY],
                 s->messagesInResendBuffer,
                 s->bytesInResendBuffer,
                 s->packetlossLastSecond * 100.0f,
                 s->packetlossTotal * 100.0f,
                 ( RakNet::GetTimeUS() - s->connectionStartTime ) / 1000000 );

        if( s->BPSLimitByCongestionControl != 0 )
        {
            char buff2[128];
            sprintf( buff2,
                     "Send capacity                    %" PRIu64 " bytes per second (%.0f%%)\n",
                     s->BPSLimitByCongestionControl,
                     100.0f * s->valueOverLastSecond[ACTUAL_BYTES_SENT] / s->BPSLimitByCongestionControl );
            strcat( buffer, buff2 );
        }
        if( s->BPSLimitByOutgoingBandwidthLimit != 0 )
        {
            char buff2[128];
            sprintf( buff2,
                     "Send limit                       %" PRIu64 " (%.0f%%)\n",
                     s->BPSLimitByOutgoingBandwidthLimit,
                     100.0f * s->valueOverLastSecond[ACTUAL_BYTES_SENT] / s->BPSLimitByOutgoingBandwidthLimit );
            strcat( buffer, buff2 );
        }
    }
}

} // namespace RakNet
