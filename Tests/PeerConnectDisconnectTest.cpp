/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "CommonFunctions.h"
#include "ConnectionWaits.h"
#include "PeerScope.h"

#include "GetTime.h"
#include "MessageIdentifiers.h"
#include "RakNetTime.h"
#include "RakNetTypes.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <sstream>
#include <thread>
#include <vector>

/*
Eight peers connect to one another, then spend ten seconds closing every
connection they hold and immediately reopening it. When the churn stops, every
peer must be holding the other seven again.

RakPeerInterface functions explicitly tested:

    Connect
    CloseConnection
    GetSystemList

Exercised indirectly by getting to that point: Startup,
SetMaximumIncomingConnections, Receive, DeallocatePacket, GetConnectionState.

The connection cap is three times the peer count, so neither limit binds and the
test is about the churn.

THIS TEST CARRIED THE SUITE'S ONE KNOWN FLAKE, ~1 run in 34 in Debug, reported as
"Failed on peer number 6 with 6 peers". It had two halves, and the two waits below
are what closed them:

  - Its all-pairs wait polled peerList[i] toward j > i only. At eight peers that
    polls peer 6 once and peer 7 never - and peer 6, which initiates to peer 7
    alone, is exactly the peer that failed; its six inbound registrations were what
    nothing waited for. ConnectionWaits::WaitForAllPairsToSettle sweeps every
    ordered pair, i outer and j inner, so both sides of every attempt have reached
    a final state when it returns (see ConnectionWaits.h).
  - Even swept in full, that wait waits for a request to SETTLE, not to SUCCEED: a
    peer whose attempt just failed satisfies it immediately, and the counts were
    then read once, straight after it. ConnectionWaits::WaitForConnectionCounts
    turns that check into a polled predicate, so the counts are read until they are
    meaningful or a ten-second deadline passes.

Nothing here is tagged, retried or widened. A missing settle window is a test
defect, and a green repeat run is not evidence that it is gone.
*/

using namespace RakNet;

namespace {

constexpr int kPeerNum = 8;
constexpr unsigned short kBasePort = 30000;

// Startup slot count and incoming limit both.
constexpr unsigned int kMaxConnections = kPeerNum * 3;

// How long to keep closing and reopening connections.
constexpr TimeMS kChurnDuration = 10000;

// After closing every connection, before reopening them: CloseConnection with a
// disconnection notification leaves the peer IS_DISCONNECTING for as long as it
// takes to send it, and a Connect issued in that window is skipped rather than
// attempted.
constexpr TimeMS kCloseSettlePause = 100;

// A floor with slack, not an expected count: the churn does 71 rounds in Release
// and 67 in Debug, and a round costs the 100 ms pause plus a settle sweep. Its
// only job is to rule out a run in which the loop churned once or not at all and
// the count wait below then reported on the initial connect - the test names ten
// seconds of closing and reopening, so it should fail if it did not do that.
constexpr int kMinimumChurnRounds = 10;

// The connection-result messages a pair's last attempt can end in, by name.
const char* ConnectionResultName( unsigned char id )
{
    switch( id )
    {
    case 0:
        return "none";
    case ID_CONNECTION_REQUEST_ACCEPTED:
        return "ID_CONNECTION_REQUEST_ACCEPTED";
    case ID_NEW_INCOMING_CONNECTION:
        return "ID_NEW_INCOMING_CONNECTION";
    case ID_CONNECTION_ATTEMPT_FAILED:
        return "ID_CONNECTION_ATTEMPT_FAILED";
    case ID_ALREADY_CONNECTED:
        return "ID_ALREADY_CONNECTED";
    case ID_NO_FREE_INCOMING_CONNECTIONS:
        return "ID_NO_FREE_INCOMING_CONNECTIONS";
    case ID_IP_RECENTLY_CONNECTED:
        return "ID_IP_RECENTLY_CONNECTED";
    case ID_DISCONNECTION_NOTIFICATION:
        return "ID_DISCONNECTION_NOTIFICATION";
    case ID_CONNECTION_LOST:
        return "ID_CONNECTION_LOST";
    default:
        return "another message";
    }
}

bool IsConnectionResult( unsigned char id )
{
    return id == ID_CONNECTION_REQUEST_ACCEPTED || id == ID_NEW_INCOMING_CONNECTION || id == ID_CONNECTION_ATTEMPT_FAILED ||
           id == ID_ALREADY_CONNECTED || id == ID_NO_FREE_INCOMING_CONNECTIONS || id == ID_IP_RECENTLY_CONNECTED ||
           id == ID_DISCONNECTION_NOTIFICATION || id == ID_CONNECTION_LOST;
}

} // namespace

// Wrap-safe on a uint32_t TimeMS, where a plain >= is not. See ConnectionWaits.h.
using ConnectionWaits::Expired;

TEST_CASE( "Eight peers closing and reopening every connection for ten seconds all end up connected to the other seven", "[network]" )
{
    PeerScope peers;

    RakPeerInterface* peerList[kPeerNum];

    for( int i = 0; i < kPeerNum; i++ )
    {
        // Server(): the Startup slot count and the incoming limit are the same
        // number here, which is exactly the pair Server() sets.
        peerList[i] = peers.Server( static_cast<unsigned short>( kBasePort + i ), kMaxConnections );
    }

    // One shape for every connect pass, whether or not anything is connected when
    // it runs - the first pass cannot skip anything.
    auto connectMissingPairs = [&]() {
        for( int i = 0; i < kPeerNum; i++ )
        {
            // From i + 1, so a pair is attempted once rather than from both ends.
            for( int j = i + 1; j < kPeerNum; j++ )
            {
                const SystemAddress target( "127.0.0.1", static_cast<unsigned short>( kBasePort + j ) );

                // Connected, connecting, pending or disconnecting: leave it be.
                if( CommonFunctions::ConnectionStateMatchesOptions( peerList[i], target, true, true, true, true ) )
                {
                    continue;
                }

                INFO( "peer " << i << " connecting to peer " << j );
                REQUIRE( peerList[i]->Connect( "127.0.0.1", kBasePort + j, 0, 0 ) == CONNECTION_ATTEMPT_STARTED );
            }
        }
    };

    // The last connection-result message each peer received from each other peer. Kept
    // so that a failure can say why a pair is missing, not only that it is.
    unsigned char lastResult[kPeerNum][kPeerNum] = {};

    auto drainRecordingResults = [&]() {
        for( int i = 0; i < kPeerNum; i++ )
        {
            for( Packet* packet = peerList[i]->Receive(); packet; peerList[i]->DeallocatePacket( packet ), packet = peerList[i]->Receive() )
            {
                const int j = packet->systemAddress.GetPort() - kBasePort;
                if( packet->length > 0 && IsConnectionResult( packet->data[0] ) && j >= 0 && j < kPeerNum )
                    lastResult[i][j] = packet->data[0];
            }
        }
    };

    connectMissingPairs();

    std::vector<SystemAddress> systemList;
    std::vector<RakNetGUID> guidList;

    int churnRounds = 0;

    const TimeMS churnDeadline = GetTimeMS() + kChurnDuration;

    while( !Expired( churnDeadline ) )
    {
        for( int i = 0; i < kPeerNum; i++ )
        {
            peerList[i]->GetSystemList( systemList, guidList );

            for( const SystemAddress& address : systemList )
            {
                peerList[i]->CloseConnection( address, true, 0, LOW_PRIORITY );
            }
        }

        std::this_thread::sleep_for( std::chrono::milliseconds( kCloseSettlePause ) );

        connectMissingPairs();

        ConnectionWaits::WaitForAllPairsToSettle( peerList, kPeerNum, kBasePort );

        // Counted after the wait, so a round is one completed close-and-reopen
        // cycle rather than one entry into the loop body.
        churnRounds++;

        // Every iteration, and this is not decoration: the churn generates a
        // connection notification per end per round, and a loop that polls without
        // draining grows its queues without bound.
        drainRecordingResults();
    }

    // The last word: whatever the churn left half-open gets one more attempt, then
    // both sides of every attempt are waited out.
    connectMissingPairs();

    ConnectionWaits::WaitForAllPairsToSettle( peerList, kPeerNum, kBasePort );

    // The count wait below does not drain - see ConnectionWaits.h - and it is the
    // longest single poll in the test, so the queues go into it empty.
    drainRecordingResults();

    // CHECK rather than REQUIRE: it reports on the loop above rather than gating
    // anything below, and it should not hide the verdict.
    CHECK( churnRounds >= kMinimumChurnRounds );

    // Every pair not connected once the final attempts have settled, with each end's
    // state and the last connection-result message it received. Only shown if the
    // count wait below fails.
    std::ostringstream missingPairs;
    for( int i = 0; i < kPeerNum; i++ )
    {
        for( int j = i + 1; j < kPeerNum; j++ )
        {
            const SystemAddress toJ( "127.0.0.1", static_cast<unsigned short>( kBasePort + j ) );
            const SystemAddress toI( "127.0.0.1", static_cast<unsigned short>( kBasePort + i ) );
            const ConnectionState iState = peerList[i]->GetConnectionState( toJ );
            const ConnectionState jState = peerList[j]->GetConnectionState( toI );
            if( iState != IS_CONNECTED || jState != IS_CONNECTED )
            {
                missingPairs << "\n  " << i << "-" << j << ": peer " << i << " state " << (int)iState << ", last "
                             << ConnectionResultName( lastResult[i][j] ) << "; peer " << j << " state " << (int)jState
                             << ", last " << ConnectionResultName( lastResult[j][i] );
            }
        }
    }
    INFO( "pairs not connected after the final attempts (state 2 is IS_CONNECTED):" << missingPairs.str() );

    // The test's verdict, and a polled predicate rather than a reading taken after
    // a wait - taking it once is half of what made this test flaky. It fails if
    // the peers never connect, listing every peer's count.
    ConnectionWaits::WaitForConnectionCounts( peerList, kPeerNum, kPeerNum - 1 );
}
