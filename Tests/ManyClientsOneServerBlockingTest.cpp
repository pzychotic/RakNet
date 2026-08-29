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

#include <chrono>
#include <thread>
#include <vector>

/*
256 clients connect to one server, then spend ten seconds closing the connection
they hold and immediately reopening it. When the churn stops, every client must be
holding exactly one connection and the server must be holding all 256.

This version waits for connects in a blocking loop, hence the name; its
non-blocking sibling is ManyClientsOneServerNonBlockingTest.

RakPeerInterface functions explicitly tested:

    Connect
    CloseConnection
    GetSystemList

Exercised indirectly by getting to that point: Startup,
SetMaximumIncomingConnections, Receive, DeallocatePacket, GetConnectionState.

The settle wait is bounded by one deadline threaded through all 256 clients, not
by a budget per client, which would bound nothing. And the connection counts are
read through WaitForConnectionCounts rather than snapshotted after the settle wait:
that wait is satisfied by a request that SETTLED and not by one that SUCCEEDED
(see ConnectionWaits.h), so a client whose attempt had just failed satisfies it
instantly and would then read as a failure.

The server's own view - the "one server" half of the name - is asserted AFTER the
clients', not before, and the ordering is worth reading carefully, because the
obvious argument for it is BACKWARDS. The server's list LAGS the clients', by one
message hop:

  - GetSystemList lists only remote systems whose connectMode is CONNECTED
    (Source/RakPeer.cpp:1475).
  - A client sets its own CONNECTED on ID_CONNECTION_REQUEST_ACCEPTED and only
    THEN sends ID_NEW_INCOMING_CONNECTION (Source/RakPeer.cpp:5497 and :5518).
  - The server sets CONNECTED when that message arrives (Source/RakPeer.cpp:5314).

So "all 256 clients report a connection" does NOT imply the server already holds
256, and a snapshot of the server taken straight after the client wait would be a
race. WaitForConnectionCounts is what makes it safe: it polls that one-hop window
out under a ten-second deadline. The assertion is not implied by the one above it
either - it fails on its own if the server never completes registration for a
client the client believes it is connected to. Each client is one address and one
address holds one entry, so the server cannot be over 256.

WHERE THE 89 SECONDS GO, because it is not where the name suggests and the next
reader should not have to measure it again: 65.1 s is building the 257 peers and
13.9 s is destroying them, against 10.1 s for the whole ten-second loop and under
0.1 s for every close, connect and drain in it. RakPeerInterface::GetInstance()
costs ~245 ms on its own - RakPeer's constructor calls GenerateGUID, which harvests
entropy from sixteen 1 ms sleeps, and a 1 ms sleep on Windows is ~15.6 ms. Nothing
here works around it.

Those are the phases of one 89.2 s in-process run; the body varies 86-91 s run to
run. As a ctest entry the test is 91.1-91.4 s in Release and 92.5-94.0 s in Debug,
the extra being the per-entry process launch and PRE_TEST discovery every entry
pays.
*/

using namespace RakNet;

namespace {

constexpr unsigned short kServerPort = 60000;
constexpr int kClientNum = 256;

// How long to keep closing and reopening connections.
constexpr TimeMS kChurnDuration = 10000;

// After closing every connection, before reopening it: CloseConnection with a
// disconnection notification leaves the client IS_DISCONNECTING for as long as it
// takes to send it, and a Connect issued in that window is skipped rather than
// attempted.
constexpr TimeMS kCloseSettlePause = 100;

// A floor with slack, not an expected count: the churn does 51-54 sweeps in
// Release and 45 in Debug - measured in both, because the floor has to hold in
// the slower one - and a sweep costs the 100 ms pause plus a settle wait that
// averages 80 ms across all 256 clients. Its only job is to rule out a run in
// which the loop swept once or not at all and the count waits below then reported
// on the initial connect - the test names ten seconds of closing and reopening, so
// it should fail if it did not do that.
//
// The loop keeps its deadline, which is worth saying because the nearest
// neighbour does not: ManyClientsOneServerDeallocateBlockingTest's "run for 30
// seconds" loop completes two sweeps, because a sweep there destroys and
// recreates 256 peers. Ten seconds here really is ten seconds - 10.1 s of an
// 89 s test, and the file header says where the other 79 s go.
constexpr int kMinimumSweeps = 10;

} // namespace

// Wrap-safe on a uint32_t TimeMS, where a plain >= is not. See ConnectionWaits.h.
using ConnectionWaits::Expired;

TEST_CASE( "256 clients closing and reopening their connection for ten seconds all end up connected to the one server", "[network][slow]" )
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

    // One shape for every connect pass, whether or not anything is connected when
    // it runs - the first pass cannot skip anything.
    auto connectIdleClients = [&]() {
        for( int i = 0; i < kClientNum; i++ )
        {
            // Connected, connecting, pending or disconnecting: leave it be.
            if( CommonFunctions::ConnectionStateMatchesOptions( clientList[i], serverAddress, true, true, true, true ) )
            {
                continue;
            }

            // REQUIRE rather than the suite's CHECK default, and the sweep loop is
            // why: this runs up to 256 times a sweep, and a client that cannot
            // start a connection attempt will not start one next sweep either, so
            // a CHECK would report one defect hundreds of times.
            INFO( "client " << i );
            REQUIRE( clientList[i]->Connect( "127.0.0.1", kServerPort, 0, 0 ) == CONNECTION_ATTEMPT_STARTED );
        }
    };

    connectIdleClients();

    std::vector<SystemAddress> systemList;
    std::vector<RakNetGUID> guidList;

    int sweeps = 0;

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

        std::this_thread::sleep_for( std::chrono::milliseconds( kCloseSettlePause ) );

        connectIdleClients();

        ConnectionWaits::WaitForRequestsToSettle( clientList, kClientNum, serverAddress );

        // Counted after the wait, so a sweep is one completed close-and-reopen
        // cycle rather than one entry into the loop body.
        sweeps++;

        // Every iteration, and this is not decoration: the churn generates a
        // connection notification per end per sweep across 257 peers, and a loop
        // that polls without draining grows its queues without bound.
        ConnectionWaits::Drain( server );
        ConnectionWaits::DrainAll( clientList, kClientNum );
    }

    // The last word: whatever the churn left half-open gets one more attempt, then
    // every attempt is waited out.
    connectIdleClients();

    ConnectionWaits::WaitForRequestsToSettle( clientList, kClientNum, serverAddress );

    // The count waits below do not drain - see ConnectionWaits.h - and they are the
    // longest polls in the test, so the queues go into them empty.
    ConnectionWaits::Drain( server );
    ConnectionWaits::DrainAll( clientList, kClientNum );

    // CHECK rather than REQUIRE: it reports on the loop above rather than gating
    // anything below, and it should not hide the verdict.
    CHECK( sweeps >= kMinimumSweeps );

    // The test's verdict, and a polled predicate rather than a reading taken after
    // the settle wait. It fails if the clients never connect, listing every
    // client's count rather than the lowest-numbered one that was short.
    ConnectionWaits::WaitForConnectionCounts( clientList, kClientNum, 1 );

    // The other half of the name. After the wait above, not before it - see the
    // file header for why that ordering is what makes this a reading rather than a
    // race.
    //
    // The INFO is not decoration either: the wait reports by index, so without it
    // a short server is reported as "peer 0", which reads as a client.
    INFO( "the server" );
    ConnectionWaits::WaitForConnectionCounts( &server, 1, kClientNum );
}
