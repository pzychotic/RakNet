/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "PeerScope.h"

#include "CommonFunctions.h"
#include "ConnectionWaits.h"
#include "GetTime.h"
#include "MessageIdentifiers.h"
#include "PacketPriority.h"
#include "Rand.h"
#include "RakNetStatistics.h"
#include "RakNetTime.h"
#include "RakNetTypes.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

/*
Ten peers on 60000-60009 spend ten seconds being shuffled by a seeded random
driver: which peer acts, which peer it targets, and whether it connects, closes,
broadcasts or sends directed traffic, pings on or offline, reads its connection
list or reads statistics. The subject is not any one of those calls. It is
whether ten peers under a continuous mixture of all of them keep a working mesh.

So that is what is asserted, at every sample of the run and once more after it:

  - Every peer is still active. Nothing here ever shuts one down.
  - Every address a peer lists is one of the other nine peers, and never itself.
  - The mesh holds at least kMinimumConnectionEnds connection ends. This is the
    assertion the test exists for and the one that catches the mesh degrading
    mid-run - see the note on the action rate below, which is how it was found.

RakPeerInterface functions explicitly tested:

    Connect
    Send
    CloseConnection
    Ping (both the offline host/port form and the online SystemAddress form)
    GetConnectionList
    GetSystemList
    GetStatistics
    GetSystemAddressFromIndex
    IsActive

Exercised indirectly by getting to that point: Startup,
SetMaximumIncomingConnections, SetOfflinePingResponse, GetConnectionState
(through CommonFunctions::ConnectionStateMatchesOptions), Receive,
DeallocatePacket, StatisticsToString.

--- The action rate, which is what makes the mesh floor assertable ---

DO NOT REMOVE THE ACTION SCHEDULE. Driving these ten peers from a bare loop whose
only pause is sleep_for( 0 ms ) - about 8.7 million passes in the ten seconds,
87,000 connect actions and 9,400 sends per second - makes the mesh peak in the
first hundred milliseconds and then vanish:

    ends per 50 ms sample, unthrottled:
    18 60 51 35 23 21 14 11 12 9 6 5 4 3 2 1 1 1 1 1 1 1 1 0 0 0 ... 0

It collapses to zero inside 1.2 s and never recovers, leaving roughly nine of the
ten seconds with ten fully disconnected peers - and with only Connect's return code
to fail on, that passes.

It is not a library defect. Three probes separate the causes: with the send branch
disabled the mesh still collapses, with the connect branch disabled it still
collapses, and with every action disabled - the same spin, the same ten drains per
pass - it holds flat for the full ten seconds. So neither channel is at fault; the
driver simply outruns what ten peers can service, on both channels independently,
and RakNet times the connections out. Nothing promises otherwise.

So actions are taken on a fixed schedule, kActionsPerInterval of them every
kActionIntervalMs. That makes the churn a churn - measured at 50,000 actions, 4,100
connect actions, 840 sends and 42 closes that found a live target - and the mesh
climbs to 75-79 ends and stays there. It also makes the run reproducible: a seed
fixes which actions are taken, but only a schedule fixes how many, and without one
the same binary on a faster machine runs a different test.

--- The random driver and its fixed seed ---

Both load-bearing, exactly as in DroppedConnectionConvert. The interleaving of
connects, closes, restarts and sends IS the coverage - a fixed script would
exercise one interleaving of it - and the seed is what makes a red run
reproducible rather than re-measurable.

randomMT is process-global, and three places in Source/ touch it:

  - ReliabilityLayer.cpp:1887 and :1905 draw from it, both inside #ifdef _DEBUG
    and both gated on packetloss / extraPingVariance, which only
    ApplyNetworkSimulator sets and this test never calls.
  - TwoWayAuthentication.cpp:117 *seeds* it, from the clock, in the plugin's
    constructor. That is the one that would matter, since it would overwrite the
    seed rather than consume draws - but it runs only if something constructs a
    TwoWayAuthentication, and nothing under Tests/ does.

So the seed does determine every draw in this process. What it does not fix is
the schedule: packet timing and the loop cadence still vary, so a replay repeats
the same sequence of actions, not the same milliseconds.

--- The empty action bands, which are load-bearing ---

DO NOT CLOSE THE GAPS in takeOneAction's thresholds. They are cumulative, so
deleting an empty band redistributes every draw above it: closing the RPC gap
promotes CloseConnection from 0.1% of draws to 4.1%, which was measured tearing
the mesh down as fast as it formed, and closing the Disconnect gap would hand
GetConnectionList half as many draws again.

--- A peer connecting to its own port ---

Drawing a connect target as randomMT() % kPeerNum with no exclusion makes one
attempt in ten a peer connecting to 127.0.0.1 on its own port. RakNet allows it:
measured, peer 0 completes the connection and then lists port 60000 - itself - in
its own system list, which fires the "never itself" assertion below with 0 != 0.

Suppressed here, deliberately. A peer connected to itself is not an edge of the
mesh this test is about, and it spends a connection slot on both ends of one peer,
where the slot count is what actually binds (see kMaxIncomingConnections). The
draw is still made, so the sequence the seed describes is unchanged; only the
attempt is skipped. Whether RakNet ought to allow a self-connection at all is a
question this test is not the place to ask.

--- What is deliberately not asserted ---

Symmetry - that if peer i lists peer j then peer j lists peer i - which
MaximumConnect asserts after the same all-pairs settle wait. It does not hold
here and the reason is not a defect: half of this test's closes are silent, and a
silently closed connection leaves the other side only when its timeout fires.
Measured at 14 asymmetric pairs of 45 after a 1 s quiesce, 10 after 3 s, and 0
after 12 s, which is the default 10 s timeout plus slack. Buying it would cost
twelve seconds for a property about close bookkeeping rather than about the mesh,
so the floor below is the degradation assertion instead.
*/

using namespace RakNet;

namespace {

constexpr int kPeerNum = 10;
constexpr unsigned short kBasePort = 60000;

// Binds far more loosely than it reads - see MaximumConnectTest's header for why
// an incoming limit of four does not hold a peer to four connections - which is
// why the mesh below reaches seventy-odd ends across ten peers rather than forty.
constexpr unsigned short kMaxIncomingConnections = 4;

constexpr TimeMS kChurnDurationMs = 10000;

// Long enough for traffic in flight when the driver stops to land, so the final
// reading is of a mesh at rest. Deliberately not long enough to age out silent
// closes; see the note on symmetry above.
constexpr TimeMS kQuiesceMs = 1000;

constexpr TimeMS kSampleIntervalMs = 50;

// The action schedule. See the header: unthrottled, this test cannot assert
// anything about its own mesh, because it does not have one after the first
// second. 5 per millisecond lands at roughly 50,000 actions in the ten seconds,
// which is enough for the 0.1% CloseConnection band to fire a few dozen times
// and few enough that the peers keep up.
constexpr TimeMS kActionIntervalMs = 1;
constexpr int kActionsPerInterval = 5;

// See the header for why the seed is load-bearing.
constexpr unsigned int kSeed = 12345;

// The send length, 3 + randomMT() % 8000, split into its two halves so the buffer
// below is derived from it rather than being a separate literal that happens to
// be larger.
constexpr int kMinSendBytes = 3;
constexpr int kSendBytesSpread = 8000;
constexpr int kMaxPayloadBytes = kMinSendBytes + kSendBytesSpread - 1;

// StatisticsToString at verbosity 0 writes three lines; this is an order of
// magnitude more than it needs.
constexpr int kStatisticsBufferBytes = 512;

// Connection ends: one connection contributes two, one to each peer holding it.
//
// The setup draws its targets from the seed and settled at 18 ends - nine
// connections - on every run measured, so this floor is there to rule out the
// empty run rather than to describe a shape.
constexpr int kMinimumConnectionEndsAfterSetup = 8;

// The degradation floor, and the assertion this test exists for. Measured at
// 63-79 ends from 200 ms into the churn through to the end of it, so this is
// better than a factor of two of slack. Unthrottled, the same reading is 0 from
// 1.2 s onward.
constexpr int kMinimumConnectionEnds = 30;

// The mesh starts the churn at the setup's 18 ends and climbs past the floor
// within about 200 ms. Two seconds of grace, so the floor is a statement about a
// mesh that has had time to form rather than a race with the ramp.
constexpr TimeMS kMeshFloorGraceMs = 2000;

// Three of the action bands draw the same pair in the same order - a peer, then
// one of that peer's connected systems - so they draw it through one lambda.
struct PeerAndTarget
{
    int peerIndex;
    RakNet::SystemAddress target;
};

} // namespace

TEST_CASE( "Ten peers survive ten seconds of random connects, closes, sends and pings with the mesh intact", "[network]" )
{
    PeerScope peers;

    RakPeerInterface* peerList[kPeerNum];

    for( int i = 0; i < kPeerNum; i++ )
    {
        // Startup slots at kPeerNum and the incoming limit at four; PeerScope::Server
        // sets both to its one argument, so the limit is set again after it.
        peerList[i] = peers.Server( static_cast<unsigned short>( kBasePort + i ), kPeerNum );
        peerList[i]->SetMaximumIncomingConnections( kMaxIncomingConnections );
        peerList[i]->SetOfflinePingResponse( "Offline Ping Data", static_cast<int>( strlen( "Offline Ping Data" ) ) + 1 );
    }

    seedMT( kSeed );

    std::vector<SystemAddress> systemList;
    std::vector<RakNetGUID> guidList;

    std::vector<char> payload( kMaxPayloadBytes, 0 );
    char statisticsBuffer[kStatisticsBufferBytes];

    auto connectIfIdle = [&]( int from, int to ) {
        const SystemAddress target( "127.0.0.1", static_cast<unsigned short>( kBasePort + to ) );

        // Connect on a live or in-flight connection queues nothing, so ask first.
        if( CommonFunctions::ConnectionStateMatchesOptions( peerList[from], target, true, true, true, true ) )
        {
            return;
        }

        const ConnectionAttemptResult result = peerList[from]->Connect( "127.0.0.1", static_cast<unsigned short>( kBasePort + to ), 0, 0 );

        // ALREADY_CONNECTED_TO_ENDPOINT is allowed, and it is reachable rather
        // than defensive: the guard above treats IS_SILENTLY_DISCONNECTING as
        // idle, while RakPeer still holds an active remote system for it and
        // answers Connect with ALREADY_CONNECTED (Source/RakPeer.cpp:2824). This
        // test closes silently on half its closes, so that window is one it opens
        // itself.
        //
        // REQUIRE rather than the suite's CHECK default, because this latches:
        // it runs about four thousand times a run, and unlike the send below, a
        // Connect returning something unexpected says nothing about the peer being
        // down - so nothing else stops the test and a CHECK would report the same
        // defect a few thousand times over.
        INFO( "peer " << from << " connecting to peer " << to << ", Connect returned " << static_cast<int>( result ) );
        REQUIRE( ( result == CONNECTION_ATTEMPT_STARTED || result == ALREADY_CONNECTED_TO_ENDPOINT ) );
    };

    // Peer first, then target, so the target is drawn from the peer that will act
    // on it. GetSystemAddressFromIndex hands back UNASSIGNED_SYSTEM_ADDRESS unless
    // that slot holds a fully connected system, so most draws come back unassigned
    // and every caller has to expect it.
    auto drawPeerAndConnectedTarget = [&]() {
        const int peerIndex = static_cast<int>( randomMT() % kPeerNum );
        const SystemAddress target = peerList[peerIndex]->GetSystemAddressFromIndex( randomMT() % kPeerNum );

        return PeerAndTarget{ peerIndex, target };
    };

    // The mesh invariants and the reading the floors are taken from, in one pass
    // so that a sample is one state rather than ten separately-timed reads of it.
    // Used by the samples during the churn and by the final reading after it.
    auto checkMeshAndCountEnds = [&]() {
        int ends = 0;

        for( int i = 0; i < kPeerNum; i++ )
        {
            // Nothing in this test ever shuts a peer down, so an inactive peer is
            // one that stopped on its own. REQUIRE, not CHECK: it latches, and it
            // makes every reading below meaningless.
            INFO( "peer " << i );
            REQUIRE( peerList[i]->IsActive() );

            peerList[i]->GetSystemList( systemList, guidList );
            ends += static_cast<int>( systemList.size() );

            for( const SystemAddress& address : systemList )
            {
                // The only ten addresses that can legitimately appear, and a peer
                // never connects to itself here. Cheap, and it is the reading that
                // would catch a system list that had become garbage rather than
                // merely short.
                const int listed = static_cast<int>( address.GetPort() ) - kBasePort;

                INFO( "peer " << i << " lists port " << address.GetPort() );
                REQUIRE( listed >= 0 );
                REQUIRE( listed < kPeerNum );
                REQUIRE( listed != i );
            }
        }

        return ends;
    };

    auto takeOneAction = [&]() {
        const float nextAction = frandomMT();

        if( nextAction < .09f )
        {
            // Connect.
            const int from = static_cast<int>( randomMT() % kPeerNum );
            const int to = static_cast<int>( randomMT() % kPeerNum );

            // A peer connecting to its own port is skipped, not undrawn - see the
            // header. The draw stands, so the sequence the seed describes is
            // unchanged.
            if( to != from )
            {
                connectIfIdle( from, to );
            }
        }
        else if( nextAction < .10f )
        {
            // Empty band. Do not close it - see the header.
        }
        else if( nextAction < .12f )
        {
            // GetConnectionList, into a caller-supplied buffer - the one call
            // site in the suite that exercises that overload rather than the
            // count-only one.
            const int peerIndex = static_cast<int>( randomMT() % kPeerNum );

            SystemAddress remoteSystems[kPeerNum];
            unsigned short numSystems = kPeerNum;

            // The return is the only thing about this call that can fail: it is
            // false when the peer has no remote system list at all. Asserting the
            // out-parameter instead would be a tautology - GetConnectionList clamps
            // it to the capacity it was handed (Source/RakPeer.cpp:943), so it
            // cannot come back above kPeerNum.
            INFO( "peer " << peerIndex );
            REQUIRE( peerList[peerIndex]->GetConnectionList( remoteSystems, &numSystems ) );

            // What the buffer overload actually has to get right: every entry it
            // wrote is one of the ten peers and never the caller itself. The
            // count-only form the sampler uses cannot ask this.
            for( unsigned short entry = 0; entry < numSystems; entry++ )
            {
                const int listed = static_cast<int>( remoteSystems[entry].GetPort() ) - kBasePort;

                INFO( "peer " << peerIndex << " entry " << entry << " is port " << remoteSystems[entry].GetPort() );
                REQUIRE( listed >= 0 );
                REQUIRE( listed < kPeerNum );
                REQUIRE( listed != peerIndex );
            }
        }
        else if( nextAction < .14f )
        {
            // Send. The sender is drawn first and the target then drawn from that
            // same peer's list, or a directed send goes to an address the sender is
            // not connected to.
            const int peerIndex = static_cast<int>( randomMT() % kPeerNum );

            const int dataLength = kMinSendBytes + static_cast<int>( randomMT() % kSendBytesSpread );
            const PacketPriority priority = static_cast<PacketPriority>( randomMT() % static_cast<int>( NUMBER_OF_PRIORITIES ) );
            const PacketReliability reliability = static_cast<PacketReliability>( randomMT() % ( static_cast<int>( RELIABLE_SEQUENCED ) + 1 ) );
            const char orderingChannel = static_cast<char>( randomMT() % 32 );

            SystemAddress target = UNASSIGNED_SYSTEM_ADDRESS;
            if( ( randomMT() % kPeerNum ) != 0 )
            {
                target = peerList[peerIndex]->GetSystemAddressFromIndex( randomMT() % kPeerNum );
            }

            const bool broadcast = ( randomMT() % 2 ) > 0;

            payload[0] = static_cast<char>( ID_USER_PACKET_ENUM );

            const uint32_t sendResult = peerList[peerIndex]->Send( payload.data(), dataLength, priority, reliability, orderingChannel, target, broadcast );

            // Send returns 0 for an undefined target that is not a broadcast
            // (Source/RakPeer.cpp:1006), which is a draw this test makes on
            // purpose one time in ten. Everything else must produce a receipt.
            //
            // CHECK, the suite's default, and it holds here rather than giving way
            // to the latching exception: every send carries a different peer,
            // length, reliability and target, so a second failure narrows the cause
            // instead of repeating the first. The count is bounded anyway - the
            // only way Send returns 0 for a defined target is a peer with no remote
            // system list, which the next sample's IsActive REQUIRE stops the test
            // on within 50 ms.
            if( broadcast || target != UNASSIGNED_SYSTEM_ADDRESS )
            {
                INFO( "peer " << peerIndex << " sending " << dataLength << " bytes" );
                CHECK( sendResult != 0 );
            }
        }
        else if( nextAction < .18f )
        {
            // Empty band. Do not close it - see the header.
        }
        else if( nextAction < .181f )
        {
            // CloseConnection, with and without a disconnection notification.
            // A tenth of a percent of draws, which is what keeps this a churn
            // rather than a teardown.
            const PeerAndTarget drawn = drawPeerAndConnectedTarget();

            peerList[drawn.peerIndex]->CloseConnection( drawn.target, ( randomMT() % 2 ) > 0, 0 );
        }
        else if( nextAction < .20f )
        {
            // Offline ping, answered out of the SetOfflinePingResponse set above.
            //
            // The bool return is deliberately not asserted. Its only false paths
            // are a null host and an address that will not parse
            // (Source/RakPeer.cpp), neither reachable from a string literal and a
            // port in range, so an assertion on it would be a tautology. What this
            // branch is for is the traffic, not the return code.
            const int peerIndex = static_cast<int>( randomMT() % kPeerNum );
            const unsigned short port = static_cast<unsigned short>( kBasePort + ( randomMT() % kPeerNum ) );

            peerList[peerIndex]->Ping( "127.0.0.1", port, ( randomMT() % 2 ) > 0 );
        }
        else if( nextAction < .21f )
        {
            // Online ping.
            const PeerAndTarget drawn = drawPeerAndConnectedTarget();

            peerList[drawn.peerIndex]->Ping( drawn.target );
        }
        else if( nextAction < .24f )
        {
            // Empty band. Do not close it - see the header.
        }
        else if( nextAction < .25f )
        {
            // GetStatistics: one peer, one of its own connections. Asking a peer
            // for statistics about its own GetInternalID always returns null - that
            // address is never a remote system.
            const PeerAndTarget drawn = drawPeerAndConnectedTarget();

            RakNetStatistics* statistics = peerList[drawn.peerIndex]->GetStatistics( drawn.target );

            // Guarded, not asserted, and the guard is the point: the address and
            // the statistics are two separate reads of remoteSystemList, and this
            // test closes about forty connections a run. A close landing between
            // them takes the entry out and returns null here with nothing wrong,
            // so a REQUIRE on non-null would be a flake rather than a check. What
            // this branch is for is running GetStatistics and StatisticsToString
            // under churn at all.
            if( statistics != 0 )
            {
                StatisticsToString( statistics, statisticsBuffer, 0 );
            }
        }
    };

    for( int i = 0; i < kPeerNum; i++ )
    {
        const int target = static_cast<int>( randomMT() % kPeerNum );

        if( target != i )
        {
            connectIfIdle( i, target );
        }
    }

    // Every ordered pair, both directions of each attempt - see ConnectionWaits.h.
    // When it returns every attempted connection has reached its final state on
    // both sides, so the count below is a reading rather than a race.
    ConnectionWaits::WaitForAllPairsToSettle( peerList, kPeerNum, kBasePort );
    ConnectionWaits::DrainAll( peerList, kPeerNum );

    // Read into a local before asserting: checkMeshAndCountEnds contains
    // REQUIREs, and a REQUIRE evaluated inside another assertion's expression is
    // evaluated inside a catch-all handler, so its throw is swallowed.
    const int endsAfterSetup = checkMeshAndCountEnds();
    REQUIRE( endsAfterSetup >= kMinimumConnectionEndsAfterSetup );

    const TimeMS entryTime = GetTimeMS();
    TimeMS nextActionTime = entryTime;
    TimeMS nextSampleTime = entryTime;

    while( GetTimeMS() - entryTime < kChurnDurationMs )
    {
        if( ConnectionWaits::Expired( nextActionTime ) )
        {
            nextActionTime = GetTimeMS() + kActionIntervalMs;

            for( int action = 0; action < kActionsPerInterval; action++ )
            {
                takeOneAction();
            }
        }

        if( ConnectionWaits::Expired( nextSampleTime ) )
        {
            nextSampleTime = GetTimeMS() + kSampleIntervalMs;

            const TimeMS elapsed = GetTimeMS() - entryTime;
            const int ends = checkMeshAndCountEnds();

            if( elapsed >= kMeshFloorGraceMs )
            {
                // REQUIRE rather than the suite's CHECK default: a collapsed mesh
                // latches, so a CHECK would report the same defect at every
                // remaining sample.
                INFO( "at " << elapsed << " ms into the churn" );
                REQUIRE( ends >= kMinimumConnectionEnds );
            }
        }

        // Every polling loop drains, or the queues grow without bound.
        ConnectionWaits::DrainAll( peerList, kPeerNum );
        std::this_thread::sleep_for( std::chrono::milliseconds( 0 ) );
    }

    // Driver stopped. Let what is in flight land before the final reading.
    const TimeMS quiesceDeadline = GetTimeMS() + kQuiesceMs;

    while( !ConnectionWaits::Expired( quiesceDeadline ) )
    {
        ConnectionWaits::DrainAll( peerList, kPeerNum );
        std::this_thread::sleep_for( std::chrono::milliseconds( 0 ) );
    }

    ConnectionWaits::WaitForAllPairsToSettle( peerList, kPeerNum, kBasePort );
    ConnectionWaits::DrainAll( peerList, kPeerNum );

    // The same floor once the churn is over, which is the difference between a
    // mesh that survived the run and one that was still standing when the last
    // sample happened to be taken.
    const int endsAtEnd = checkMeshAndCountEnds();
    REQUIRE( endsAtEnd >= kMinimumConnectionEnds );
}
