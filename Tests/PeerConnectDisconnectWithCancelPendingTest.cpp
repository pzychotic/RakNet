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
#include "RakNetStringMakers.h"

#include "GetTime.h"
#include "RakNetTime.h"
#include "RakNetTypes.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>
#include <vector>

/*
Eight peers connect to one another, then spend ten seconds closing every
connection they hold and reopening it - and on every round, before reopening,
each peer cancels a connection attempt it has outstanding and the cancel is
checked. When the churn stops, every peer must be holding the other seven again.

RakPeerInterface functions explicitly tested:

    CancelConnectionAttempt
    Connect
    CloseConnection
    GetConnectionState
    GetSystemList

Exercised indirectly by getting to that point: Startup,
SetMaximumIncomingConnections, Receive, DeallocatePacket.

This is PeerConnectDisconnect plus the cancel, and everything the two have in
common is deliberately identical in both files: the same eight peers on
60000-60007, the same connection cap of three times the peer count, the same
ten-second churn, the same shared waits.

THE CANCEL IS THE POINT OF THIS TEST, so it is given something to cancel and
asserted on unconditionally. Each round, before the reconnect, every peer issues
one connection attempt toward a port nothing binds and cancels it:

  - An attempt toward a live peer is not reliably pending when we look at it. It
    completes in milliseconds on loopback, and if the request packet has already
    gone out, cancelling our end leaves the far end holding a half-open handshake
    that the next round then has to unpick.
  - An attempt toward a port nothing answers stays pending until something takes
    it out of the queue. Connect pushes onto RakPeer's requestedConnectionQueue and
    returns CONNECTION_ATTEMPT_STARTED in the same call, and membership of that
    queue is exactly what GetConnectionState reports as IS_PENDING
    (RakPeer::SendConnectionRequest, RakPeer::GetConnectionState), so the attempt
    is pending before the cancel by construction rather than by timing.
  - RakPeer eventually drops an unanswered attempt itself, which would leave the
    same IS_NOT_CONNECTED behind and make the assertion pass for the wrong reason.
    The attempt is therefore given a lifetime thirty times the budget the cancel is
    watched over, so the cancel is the only thing that can end it inside the window
    - see kUnboundAttemptIntervalMS here and ConnectionWaits::kCancelBudget.

The two halves stay separately observable, which is the reason not to fold them
into one predicate:

  - the pending attempt is gone after the cancel - the per-peer REQUIRE below,
    which names the peer and the state it was left in; and
  - the reconnect still succeeds - the Connect assertion in connectMissingPairs
    and, at the end, ConnectionWaits::WaitForConnectionCounts.

A failure in the first says the cancel did nothing. A failure in the second says
the reconnect failed. Collapsed together they would say only that something went
wrong, which is the one distinction this test exists to draw.

What the test asserts if the peers never connect: WaitForConnectionCounts fails,
listing every peer's actual count. What it asserts if the cancel never works: the
per-round REQUIRE fails on the first peer left IS_PENDING. Neither can pass
vacuously - the cancel assertion runs on an attempt the test has just created and
just checked is pending, so there is always something for it to be about.

DO NOT ADD A PEER-DIRECTED CANCEL SWEEP - cancelling peerList[i] toward every
j > i before the reconnect. It looks like the natural place for the cancel and it
is provably a no-op: WaitForAllPairsToSettle closes every round, so every pair has
reached a final state before the next one begins, and a settled pair is never
IS_PENDING (the settle wait loops on exactly IS_CONNECTING and IS_PENDING and
returns only when neither holds). The sweep would have an empty queue to work on,
and any assertion on it could not fail. What such a sweep would defend against - a
stale attempt refusing the next Connect with CONNECTION_ATTEMPT_ALREADY_IN_PROGRESS
- is caught rather than prevented: that refusal fails the Connect assertion in
connectMissingPairs and names the pair.
*/

using namespace RakNet;

namespace {

constexpr int kPeerNum = 8;
constexpr unsigned short kBasePort = 60000;

// Startup slot count and incoming limit both.
constexpr unsigned int kMaxConnections = kPeerNum * 3;

// The cancel target. One past the last peer's port, so this test never binds it -
// which is the whole property being relied on. If some unrelated process on the
// machine did hold it, the attempt would connect instead of staying pending and
// the assertion below would fail naming IS_CONNECTED; a false pass is not
// reachable from here.
constexpr unsigned short kUnboundPort = kBasePort + kPeerNum;

// How long to keep closing and reopening connections.
constexpr TimeMS kChurnDuration = 10000;

// After closing every connection, before reopening them: CloseConnection with a
// disconnection notification leaves the peer IS_DISCONNECTING for as long as it
// takes to send it, and a Connect issued in that window is skipped rather than
// attempted.
constexpr TimeMS kCloseSettlePause = 100;

// How long the cancel target's attempt would live if nothing cancelled it, spelled
// out rather than left at Connect's defaults of 12 x 500 ms. RakPeer gives up on a
// request after sendConnectionAttemptCount + 1 sends spaced this far apart and
// drops it from the queue itself, and a self-expiring attempt is indistinguishable
// from a cancelled one by the time anything looks. Sixty seconds puts that thirty
// times beyond the budget below, so nothing but the cancel can end the attempt
// inside the window the assertion watches.
//
// sendConnectionAttemptCount is left at the default rather than lowered: RakPeer
// divides by sendConnectionAttemptCount / NUM_MTU_SIZES when picking an MTU, so a
// count under three divides by zero.
//
// The margin is not hypothetical. Against Connect's default six-second lifetime,
// deleting the body of CancelConnectionAttempt left the cancel assertion GREEN,
// because RakPeer dropped the attempt itself and left behind exactly what a cancel
// leaves.
constexpr unsigned kUnboundAttemptCount = 12;
constexpr unsigned kUnboundAttemptIntervalMS = 5000;

// A floor with slack, not an expected count: measured at 57 rounds in Release and
// 55 in Debug, where a round costs the 100 ms pause plus a cancel and a settle
// sweep. Its only job is to rule out a run in which the loop churned once or not
// at all - in which case the count wait at the end would be reporting on the
// initial connect, and the cancel would have been exercised once.
constexpr int kMinimumChurnRounds = 10;

} // namespace

// Wrap-safe on a uint32_t TimeMS, where a plain >= is not. See ConnectionWaits.h.
using ConnectionWaits::Expired;

TEST_CASE( "Eight peers cancelling a pending connection attempt before each reconnect all end up connected to the other seven", "[network]" )
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

                // The reconnect half of the verdict.
                INFO( "peer " << i << " connecting to peer " << j );
                REQUIRE( peerList[i]->Connect( "127.0.0.1", kBasePort + j, 0, 0 ) == CONNECTION_ATTEMPT_STARTED );
            }
        }
    };

    // The cancel half of the verdict. Every peer creates one pending attempt,
    // cancels it, and is checked. See the header comment for why the target is a
    // port nothing binds rather than another peer.
    auto cancelAPendingAttempt = [&]() {
        const SystemAddress unbound( "127.0.0.1", kUnboundPort );

        for( int i = 0; i < kPeerNum; i++ )
        {
            INFO( "peer " << i << " attempting the unbound port " << kUnboundPort );

            REQUIRE( peerList[i]->Connect( "127.0.0.1", kUnboundPort, 0, 0, nullptr, 0, kUnboundAttemptCount, kUnboundAttemptIntervalMS ) == CONNECTION_ATTEMPT_STARTED );

            // Not decoration: it is the precondition the cancel assertion below is
            // about, and asserting it here is what makes that assertion impossible
            // to satisfy vacuously.
            REQUIRE( peerList[i]->GetConnectionState( unbound ) == IS_PENDING );

            peerList[i]->CancelConnectionAttempt( unbound );
        }

        // The settle window CancelConnectionAttempt needs and does not provide,
        // and the wait that reports the cancel simply not happening. Everything
        // below it runs only if every attempt did leave the queue.
        ConnectionWaits::WaitForAttemptsToBeCancelled( peerList, kPeerNum, unbound );

        for( int i = 0; i < kPeerNum; i++ )
        {
            const ConnectionState afterCancel = peerList[i]->GetConnectionState( unbound );

            INFO( "peer " << i << " after cancelling its attempt toward the unbound port " << kUnboundPort );

            // The half the wait above cannot assert: it says the attempt left the
            // queue, this says it left it for the right reason. IS_NOT_CONNECTED
            // is what a cancelled attempt leaves behind, and naming it rules out
            // the attempt having succeeded instead, which would otherwise read as
            // a cancel that worked.
            //
            // REQUIRE rather than the suite's CHECK default, because this latches:
            // a cancel that has stopped working reports the same defect on every
            // one of the remaining rounds.
            REQUIRE( afterCancel == IS_NOT_CONNECTED );
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

        // The close sweep above puts a disconnection notification per end into the
        // queues, and the cancel wait below polls without draining - as every wait
        // in ConnectionWaits does. So the queues go into it empty, which is the
        // same arrangement the count wait at the bottom of this file gets.
        ConnectionWaits::DrainAll( peerList, kPeerNum );

        cancelAPendingAttempt();

        connectMissingPairs();

        ConnectionWaits::WaitForAllPairsToSettle( peerList, kPeerNum, kBasePort );

        // Counted after the wait, so a round is one completed cancel-and-reopen
        // cycle rather than one entry into the loop body.
        churnRounds++;

        // Every iteration, and this is not decoration: the churn generates a
        // connection notification per end per round, and a loop that polls without
        // draining grows its queues without bound.
        ConnectionWaits::DrainAll( peerList, kPeerNum );
    }

    // The last word: whatever the churn left half-open gets one more attempt, then
    // both sides of every attempt are waited out.
    connectMissingPairs();

    ConnectionWaits::WaitForAllPairsToSettle( peerList, kPeerNum, kBasePort );

    // The count wait below does not drain - see ConnectionWaits.h - and it is the
    // longest single poll in the test, so the queues go into it empty.
    ConnectionWaits::DrainAll( peerList, kPeerNum );

    // CHECK rather than REQUIRE: it reports on the loop above rather than gating
    // anything below, and it should not hide the verdict.
    CHECK( churnRounds >= kMinimumChurnRounds );

    // The reconnect half of the verdict, and a polled predicate rather than a
    // reading taken after a fixed sleep. It fails if the peers never connect,
    // listing every peer's count.
    ConnectionWaits::WaitForConnectionCounts( peerList, kPeerNum, kPeerNum - 1 );
}
