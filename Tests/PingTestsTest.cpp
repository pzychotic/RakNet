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
Measures ping statistics over loopback twice: once driving Ping by hand with
occasional pings switched off, and once leaving RakNet to ping on its own. Each
sender gets its own peer so the second measurement starts from a statistics table
of its own rather than inheriting the first's - which is what makes the second
measurement the slow one, because a fresh table is not an empty table. See
WaitForPingTableTurnover.

RakPeerInterface functions explicitly tested:

    GetAveragePing
    GetLastPing
    GetLowestPing
    SetOccasionalPing

Exercised indirectly by getting to that point: Startup,
SetMaximumIncomingConnections, Receive, DeallocatePacket, Ping.

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

// How often RakPeer::Update pings each connected system once occasional ping is
// on (Source/RakPeer.cpp:5235). Not config dependent.
constexpr int kOccasionalPingIntervalMs = 5000;

// One occasional ping per interval into a ring of PING_TIMES_ARRAY_SIZE entries,
// so this is how long it takes occasional ping to have written over every entry
// a fresh connection starts out with. Derived from the mechanism rather than
// tuned: nothing here is a knob to widen when the test goes red.
constexpr int kPingTableTurnoverMs = PING_TIMES_ARRAY_SIZE * kOccasionalPingIntervalMs;

// Called for both measurement phases, so it says which one it is speaking
// about. Two questions, not one: a negative average means the counter itself
// is broken, a large one means the link is.
void CheckAveragePing( int averagePing, const char* phase )
{
    INFO( "phase: " << phase );

    CHECK( averagePing >= 0 );
    CHECK( averagePing <= kMaxAveragePingMs );
}

// A fresh connection's ping table does not start out holding pings.
// RakPeer::OnConnectedPong runs out of ID_CONNECTION_REQUEST_ACCEPTED
// (Source/RakPeer.cpp:5481) and that same handler pings immediately, so entries
// 0 and 1 are connection setup cost, not link cost: measured over loopback they
// come back at 15-17 ms against a steady-state ping of 0-1 ms, in Debug and
// Release alike.
//
// The first phase never notices, because its ~50 manual pings write over the
// whole ring several times before it reads anything. The second phase issues no
// pings of its own, so reading the average at the end of a fixed window read
// ( 15 + 15 + firstPing ) / 3 - which is 10 when the setup samples come back
// 15 ms and 11 when either comes back 16, making the budget a coin toss decided
// by one millisecond of handshake. Measured at 13 failures in 60 Debug runs and
// 3 in 60 Release runs.
//
// So wait for occasional ping to have written over those entries, then read once.
// Deliberately not a poll on `average <= kMaxAveragePingMs`: past the turnover the
// reading is meaningful by construction, and polling the budget the caller is
// about to assert on would only trade an honest over-budget reading for a later
// one that happens to pass.
void WaitForPingTableTurnover( RakPeerInterface* sender, RakPeerInterface* receiver )
{
    RakTimer turnover( kPingTableTurnoverMs );

    while( !turnover.IsExpired() )
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
    // loop issues. The second phase turns it back on and measures that instead.
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

    CheckAveragePing( sender2->GetAveragePing( receiverAddress ), "manual pings, occasional ping off" );

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
    // Its statistics do not start empty, though - see WaitForPingTableTurnover.
    // Closed once, then waited for; never re-issued per poll - see
    // ConnectionWaits::WaitForDisconnect.
    sender2->CloseConnection( receiverAddress, true, 0, LOW_PRIORITY );
    ConnectionWaits::WaitForDisconnect( sender2, receiverAddress );

    REQUIRE( TestHelpers::WaitAndConnectTwoPeersLocally( sender, receiver, 5000 ) );

    sender->SetOccasionalPing( true );

    // A longer window than the first phase, and for a different reason: this one
    // sends nothing of its own, so it waits for occasional ping rather than for
    // enough of its own pings.
    WaitForPingTableTurnover( sender, receiver );

    CheckAveragePing( sender->GetAveragePing( receiverAddress ), "occasional ping on, no manual pings" );
}
