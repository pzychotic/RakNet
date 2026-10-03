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
#include "RakNetTypes.h"
#include "RakPeerInterface.h"
#include "RakTimer.h"
#include "TestHelpers.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

/*
Checks ping over loopback in two phases: once driving Ping by hand with
occasional pings switched off, and once leaving RakNet to ping on its own. Each
sender gets its own peer so the second phase starts from a statistics table of
its own rather than inheriting the first's.

The first phase checks the statistics: average, last and lowest over ~50 pings.
The second checks only that occasional ping pings: it sends pings on its own and
their results land in the table. It reads one sample, GetLastPing, and fails if
SetOccasionalPing( true ) does nothing, in Debug and Release alike. One sample is
enough because the first phase already covers the average arithmetic, and because
the value alone says where the sample came from - see kMaxOccasionalPingMs.

Two things keep that sample honest. The sender's timeout is raised so RakPeer's
keepalive ping, whose pong lands in the same table, cannot fall inside the wait:
at the default Release timeout it would pass the second phase on its own, with
occasional ping broken - see kSenderTimeoutMs. And the wait is a fixed one
occasional-ping interval plus slack rather than a poll on GetLastPing, because
polling the value about to be asserted would only wait out an over-budget reading
- see WaitForOneOccasionalPing.

RakPeerInterface functions explicitly tested:

    GetAveragePing
    GetLastPing
    GetLowestPing
    SetOccasionalPing

Exercised indirectly by getting to that point: Startup,
SetMaximumIncomingConnections, Receive, DeallocatePacket, Ping, SetTimeoutTime.

Ping is also covered by CrossConnectionConvertTest; SetOfflinePingResponse and
GetOfflinePingResponse by OfflineMessagesConvertTest.
*/

using namespace RakNet;

namespace {

constexpr unsigned short kReceiverPort = 60000;

// Localhost. Command line pings to 127.0.0.1 typically come back under 1 ms,
// so 10 ms is already a wide allowance and 100 ms is a stuck-somewhere check.
// Measured against a settled ping table the average sits at 0-1 ms in Debug and
// Release alike, so this one number covers both configs.
constexpr int kMaxAveragePingMs = 10;
constexpr int kMaxLowestPingMs = 10;
constexpr int kMaxLastPingMs = 100;

constexpr int kMeasureWindowMs = 1500;

// A fresh connection's ping table does not start out empty. Entry 0 is the pong
// carried by ID_CONNECTION_REQUEST_ACCEPTED, and entry 1 the pong to the ping
// RakPeer sends straight after handling it. Both are connection setup cost, not
// link cost: over loopback they read 15-17 ms against a steady-state 0-1 ms, in
// Debug and Release alike.
//
// This bound sits below that on purpose. A last ping at or under it cannot be a
// setup entry, so it came from a ping sent after the handshake, and with the
// keepalive shut out (see kSenderTimeoutMs) only occasional ping sends one.
// kMaxLastPingMs would let a broken occasional ping pass on entry 1.
constexpr int kMaxOccasionalPingMs = 10;

// How often RakPeer::Update pings each connected system once occasional ping is
// on. Not config dependent. A fresh connection's next ping time is 0, so the first
// occasional ping goes out on the first update after SetOccasionalPing( true ),
// and the next one an interval later.
constexpr int kOccasionalPingIntervalMs = 5000;

// RakPeer::Update also sends a reliable keepalive ping to a connection that has had
// no reliable send for half its timeout, and its pong lands in the same table. The
// default timeout is 10 s in Release, which puts a keepalive inside the wait
// below, and its pong alone would pass the second phase with occasional ping
// broken. A 60 s timeout puts the first keepalive at 30 s, well past the wait.
// Only the sender's timeout matters, because the receiver's keepalive pongs land
// in its own table.
constexpr RakNet::TimeMS kSenderTimeoutMs = 60000;

// Pings go out on the update cycle, so a second covers the ping and its pong many
// times over and is far short of another interval.
constexpr int kOccasionalPingSlackMs = 1000;

// Two questions, not one: a negative average means the counter itself is broken,
// a large one means the link is.
void CheckAveragePing( int averagePing )
{
    CHECK( averagePing >= 0 );
    CHECK( averagePing <= kMaxAveragePingMs );
}

// Waits one occasional-ping interval plus slack, so the ping sent an interval
// after switching occasional ping on has had its pong. The interval is what
// occasional ping promises; the ping that goes out straight away only follows
// from a fresh connection's next ping time. A fixed wait derived from the
// mechanism, not tuned, and deliberately not a poll on GetLastPing: a single
// sample against a deadline has no positive signal of its own, and polling the
// value the caller is about to assert on would only trade an honest over-budget
// reading for a later one that happens to pass.
void WaitForOneOccasionalPing( RakPeerInterface* sender, RakPeerInterface* receiver )
{
    RakTimer window( kOccasionalPingIntervalMs + kOccasionalPingSlackMs );

    while( !window.IsExpired() )
    {
        ConnectionWaits::Drain( receiver );
        ConnectionWaits::Drain( sender );

        std::this_thread::sleep_for( std::chrono::milliseconds( 30 ) );
    }
}

} // namespace

TEST_CASE( "Ping statistics over loopback stay within a millisecond budget, pinged by hand and occasionally", "[network]" )
{
    PeerScope peers;

    RakPeerInterface* sender = peers.Client();
    RakPeerInterface* sender2 = peers.Client();

    // Startup( 2, ... ) plus SetMaximumIncomingConnections( 2 ): both senders
    // connect to it, one after the other.
    RakPeerInterface* receiver = peers.Server( kReceiverPort, 2 );

    const SystemAddress receiverAddress( "127.0.0.1", kReceiverPort );

    REQUIRE( TestHelpers::WaitAndConnectTwoPeersLocally( sender2, receiver, 5000 ) );

    // Occasional ping off, so the numbers below come only from the pings this
    // loop issues. The second phase turns it on and checks that instead.
    sender2->SetOccasionalPing( false );

    RakTimer timer( kMeasureWindowMs );
    TimeMS nextPing = 0;

    while( !timer.IsExpired() )
    {
        ConnectionWaits::Drain( receiver );
        ConnectionWaits::Drain( sender2 );

        if( GetTimeMS() > nextPing )
        {
            sender2->Ping( receiverAddress );
            nextPing = GetTimeMS() + 30;
        }

        std::this_thread::sleep_for( std::chrono::milliseconds( 3 ) );
    }

    CheckAveragePing( sender2->GetAveragePing( receiverAddress ) );

    const int lastPing = sender2->GetLastPing( receiverAddress );
    const int lowestPing = sender2->GetLowestPing( receiverAddress );

    CHECK( lastPing <= kMaxLastPingMs );

    // Over loopback the lowest ping should have dropped into single digits at
    // least once across 50-odd pings.
    CHECK( lowestPing <= kMaxLowestPingMs );

    // Not a timing claim but a consistency one: whatever the link is doing, the
    // most recent ping cannot be below the lowest ever recorded.
    CHECK( lastPing >= lowestPing );

    // Second phase on a second peer, so none of the pings above are counted twice.
    // Its statistics do not start empty, though - see kMaxOccasionalPingMs.
    // Closed once, then waited for; never re-issued per poll - see
    // ConnectionWaits::WaitForDisconnect.
    sender2->CloseConnection( receiverAddress, true, 0, LOW_PRIORITY );
    ConnectionWaits::WaitForDisconnect( sender2, receiverAddress );

    REQUIRE( TestHelpers::WaitAndConnectTwoPeersLocally( sender, receiver, 5000 ) );

    sender->SetTimeoutTime( kSenderTimeoutMs, receiverAddress );
    sender->SetOccasionalPing( true );

    // This phase sends nothing of its own, so it waits for occasional ping rather
    // than for enough of its own pings.
    WaitForOneOccasionalPing( sender, receiver );

    CHECK( sender->GetLastPing( receiverAddress ) <= kMaxOccasionalPingMs );
}
