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
#include "RakNetTime.h"
#include "RakNetTypes.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <sstream>
#include <vector>

/*
256 clients connect to one server and then spend ten seconds closing the
connection they hold and immediately reopening it, as fast as the loop will go.
When the churn stops they get a fixed budget and nothing else - two seconds to
let the churn's own traffic land, one connect pass, five seconds to connect -
during which the test does nothing but pump Receive. Then every client must be
holding exactly one connection and the server all 256.

RakPeerInterface functions explicitly tested:

    Connect
    CloseConnection
    GetSystemList

Exercised indirectly by getting to that point: Startup,
SetMaximumIncomingConnections, Receive, DeallocatePacket, GetConnectionState.

WHAT THIS TESTS THAT ManyClientsOneServerBlockingTest DOES NOT, which is the whole
reason the suite carries both. ONE thing carries it:

  - THE STIMULUS. THE CHURN HAS NO SETTLE WINDOW ANYWHERE IN IT, so the same
    assertions are made against a far harsher system. The blocking sibling pauses
    100 ms after its close pass and then calls WaitForRequestsToSettle, so its
    connect pass reads a quiesced system, 51 times in ten seconds. Here the connect
    pass follows the close pass immediately and the same ten seconds buy ~46,700
    reconnects against the sibling's ~12,750. The state check and the Connect on
    the line after it therefore straddle a system that never quiesces. The REQUIRE
    on Connect's return value is the same line in both files and a different claim
    in this one, and so is the verdict it leads to. ADDING A SETTLE WAIT TO THE
    LOOP BELOW WOULD COLLAPSE THE PAIR INTO ONE TEST.

Two refinements come with it, neither of which would earn a second entry alone:
the clients and the server are read microseconds apart rather than by two
WaitForConnectionCounts calls each exiting when its own poll came good (tighter,
not different in kind - after the churn stops nothing closes a connection, so the
sibling's client counts latch); and the recovery is bounded at five seconds of a
bare receive pump with no early exit, against the sibling's ten-second polled
budget, which is why the verdict below is a snapshot rather than a wait.

WHY THE LOOP FLOOR COUNTS RECONNECTS AND NOT SWEEPS, which is the one place the
two files should NOT be read across. The blocking sibling counts sweeps because
its settle wait makes a sweep a completed close-and-reopen cycle. Here a sweep is
whatever the loop got through, and measured, the ten seconds do ~768,000 sweeps
against ~46,700 reconnects - one Connect issued per sixteen iterations, because
the overwhelming majority of iterations find every client connected, connecting
or disconnecting and do nothing at all. A floor on sweeps in this file would be a
floor on how fast the machine spins.

WHERE THE TIME WENT, since it was not the loops. Of a 99.4-100.0 s body, 68.0 s
was building the 257 peers and 14.5 s destroying them, against 10.0 s of churn and
7.0 s of fixed recovery window. RakPeerInterface::GetInstance() cost ~245 ms on its
own - RakPeer's constructor called GenerateGUID, which harvested entropy from
sixteen 1 ms sleeps, and a 1 ms sleep on Windows is ~15.6 ms.

Debug and Release used to finish within 0.5 s of each other - 99.4-100.0 s against
99.8-99.9 s, an inflation of 1.00 - which followed from the paragraph above: four
fifths of the runtime was sleeps and fixed windows, and neither gets slower without
optimisation.

Both of those are stale now. GenerateGUID draws from the operating system's random
number source, GetInstance() costs ~0.004 ms, and peer construction is no longer
the bulk of this test - so the Debug/Release inflation should be a normal one
again. The old numbers are kept as the baseline the fix is measured against.
Do not read them as current.
*/

using namespace RakNet;

namespace {

constexpr unsigned short kServerPort = 60000;
constexpr int kClientNum = 256;

// How long to keep closing and reopening connections.
constexpr TimeMS kChurnDuration = 10000;

// Drain-only window between the churn and the final connect pass. It is
// load-bearing rather than padding: the connect pass below skips a client that is
// IS_DISCONNECTING,
// and the churn's last close pass leaves up to 256 of them in exactly that
// state. Without this window those clients are skipped, finish disconnecting
// with nothing left to reconnect them, and read as zero at the end. Measured, the
// window does its job with room to spare - by the time it ends, 36-108 of 256
// clients in Release and 205-208 in Debug are idle and get their connect, the
// rest having already reconnected inside it.
constexpr TimeMS kDisconnectSettleWindow = 2000;

// Drain-only window between the final connect pass and the verdict, and the one
// budget in this file the verdict actually leans on.
//
// THIS IS A DELIBERATE DEVIATION from the suite's rule that a test reads a value
// only once that value is meaningful. Polling the assertion is what the blocking
// sibling does and is exactly what this test exists not to do, and there is no
// accidental pass to guard against - a client is either holding a connection or it
// is not. What replaces the wait is a fixed window and a measurement of how much
// of it is actually needed.
//
// Measured margin, from instrumented runs that polled the counts through the
// window: all 256 clients are back to one connection 19-28 ms after the connect
// pass in Release and 23-29 ms in Debug, and the server reaches 256 at 30-43 ms
// and 66-70 ms respectively. Five seconds is a factor of ~70 on the slowest of
// those readings.
//
// Deliberately not tightened onto that measurement: the number is an upper bound
// the test asserts, and an upper bound with seventy times the slack fails on a
// recovery that has genuinely broken rather than on a busy machine.
constexpr TimeMS kRecoveryWindow = 5000;

// A floor with slack, not an expected count, and it counts reconnects rather
// than loop iterations for the reason in the header. The churn issues
// 46,537-46,844 Connects in Release and 25,408-26,815 in Debug - measured in
// both, because the floor has to hold in the slower one - so ten per client is
// just under a factor of ten on the WORST Debug reading, which is the number
// that matters and not the range's top.
//
// Its job is to rule out a run in which the churn reconnected nothing and the
// verdict below then reported on the initial connect: the test names ten seconds
// of closing and reopening, so it should fail if it did not do that.
constexpr int kMinimumReconnects = 10 * kClientNum;

} // namespace

// Wrap-safe on a uint32_t TimeMS, where a plain >= is not. See ConnectionWaits.h.
using ConnectionWaits::Expired;

TEST_CASE( "256 clients closing and reopening their connection as fast as they can are all connected again five seconds after the churn stops", "[network]" )
{
    PeerScope peers;

    // The server first: creation order is teardown order reversed, and destroying
    // 256 connected clients before the server they are connected to is the order
    // PeerScope is written around.
    RakPeerInterface* server = peers.Server( kServerPort, kClientNum );

    RakPeerInterface* clientList[kClientNum];

    for( int i = 0; i < kClientNum; i++ )
    {
        clientList[i] = peers.Client();
    }

    const SystemAddress serverAddress( "127.0.0.1", kServerPort );

    int connectsIssued = 0;

    // One shape for every connect pass, whether or not anything is connected when
    // it runs - the first pass cannot skip anything.
    auto connectIdleClients = [&]() {
        for( int i = 0; i < kClientNum; i++ )
        {
            // Connected, connecting, pending or disconnecting: leave it be. Under
            // this churn that is the common case rather than the exception - a
            // client closed on the pass above is IS_DISCONNECTING when this runs
            // microseconds later, and gets its Connect a sweep or two afterwards.
            if( CommonFunctions::ConnectionStateMatchesOptions( clientList[i], serverAddress, true, true, true, true ) )
            {
                continue;
            }

            // REQUIRE rather than the suite's CHECK default, and the churn loop is
            // why: this runs up to 256 times a sweep across hundreds of thousands
            // of sweeps, and a client that cannot start a connection attempt will
            // not start one next sweep either, so a CHECK would report the same
            // defect tens of thousands of times.
            INFO( "client " << i );
            REQUIRE( clientList[i]->Connect( "127.0.0.1", kServerPort, 0, 0 ) == CONNECTION_ATTEMPT_STARTED );

            connectsIssued++;
        }
    };

    // A fixed window with nothing in it but the drain, which is the non-blocking
    // half of the test: the peers get wall-clock time and a pump, and nothing
    // waits on them.
    //
    // Deliberately NOT a wait, which is why it has a deadline and no failure at
    // it: every wait in this suite fails loudly at its bound naming what it was
    // waiting for, and this has nothing to wait for. Reaching the deadline is the
    // only way out and is the normal path. The loud failure is the CHECK after
    // the second call, and turning this into a predicate wait is exactly the
    // change the header argues against.
    auto pumpUntil = [&]( TimeMS deadline ) {
        while( !Expired( deadline ) )
        {
            ConnectionWaits::Drain( server );
            ConnectionWaits::DrainAll( clientList, kClientNum );
        }
    };

    connectIdleClients();

    const int connectsBeforeChurn = connectsIssued;

    std::vector<SystemAddress> systemList;
    std::vector<RakNetGUID> guidList;

    const TimeMS churnDeadline = GetTimeMS() + kChurnDuration;

    while( !Expired( churnDeadline ) )
    {
        for( int i = 0; i < kClientNum; i++ )
        {
            clientList[i]->GetSystemList( systemList, guidList );

            for( const SystemAddress& address : systemList )
            {
                clientList[i]->CloseConnection( address, true, 0, LOW_PRIORITY );
            }
        }

        // No pause and no settle wait between the two passes, which is the
        // difference from the blocking sibling and the subject of this test.
        connectIdleClients();

        // Every iteration, and this is not decoration: the churn generates a
        // connection notification per end per reconnect across 257 peers, and a
        // loop that polls without draining grows its queues without bound. At
        // ~766,000 iterations this is the loop in the suite least able to survive
        // skipping it.
        ConnectionWaits::Drain( server );
        ConnectionWaits::DrainAll( clientList, kClientNum );
    }

    const int churnReconnects = connectsIssued - connectsBeforeChurn;

    // CHECK rather than REQUIRE: it reports on the loop above rather than gating
    // anything below, and it should not hide the verdict.
    CHECK( churnReconnects >= kMinimumReconnects );

    // Let the churn's own traffic land before asking anyone to connect - see
    // kDisconnectSettleWindow.
    pumpUntil( GetTimeMS() + kDisconnectSettleWindow );

    // The last word: one attempt for whatever the churn left idle, then a fixed
    // budget to complete it in.
    connectIdleClients();

    pumpUntil( GetTimeMS() + kRecoveryWindow );

    // The verdict, and it is a snapshot on purpose rather than by inheritance.
    // The blocking sibling polls this with WaitForConnectionCounts, twice, and
    // each call exits at whatever moment its own poll came good; here both
    // readings are taken microseconds apart, so the client claim and the server
    // claim below are one consistent reading of the whole system rather than two
    // readings of two moments. That, plus the fixed budget above, is what the
    // pair does not share - see the header.
    //
    // Every client is read before anything is reported, so a failure names all of
    // them rather than the lowest-numbered one that was short. One assertion and
    // not one per client: 256 identical failures report nothing the first does not.
    std::ostringstream report;
    int clientsHoldingOne = 0;

    for( int i = 0; i < kClientNum; i++ )
    {
        clientList[i]->GetSystemList( systemList, guidList );

        const int connections = static_cast<int>( systemList.size() );

        if( connections == 1 )
        {
            clientsHoldingOne++;
        }
        else
        {
            report << "\n  client " << i << ": " << connections;
        }
    }

    INFO( "clients not holding exactly one connection:" << report.str() );
    CHECK( clientsHoldingOne == kClientNum );

    // The other half of the name. Read at the same instant as the clients and not
    // before them: GetSystemList lists only remote
    // systems at connectMode CONNECTED (Source/RakPeer.cpp:1475), a client sets
    // its own CONNECTED on ID_CONNECTION_REQUEST_ACCEPTED and only THEN sends
    // ID_NEW_INCOMING_CONNECTION (:5497, :5518), and the server sets CONNECTED
    // when that arrives (:5314) - so the server's list LAGS the clients' by one
    // message hop and is the reading with less margin, not more. Measured, that
    // hop is the difference between 23 ms and 66 ms into a five-second window.
    //
    // Each client is one address and one address holds one entry, so the server
    // cannot be over 256 either.
    server->GetSystemList( systemList, guidList );

    const int serverConnections = static_cast<int>( systemList.size() );

    CHECK( serverConnections == kClientNum );
}
