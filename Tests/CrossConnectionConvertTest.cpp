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
#include "ConnectionWaits.h"

#include "BitStream.h"
#include "GetTime.h"
#include "MessageIdentifiers.h"
#include "RakNetTime.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

/*
Tests what happens if two instances of RakNet connect to each other at the same
time. This has caused handshaking problems in the past.

The client pings the server offline, both sides use the round trip to aim their
Connect() calls at the same instant, and two seconds later each must have seen a
connection notification. Then both sides tear the connection down and the round
starts over, for as many rounds as fit in ten seconds.

RakPeerInterface functions explicitly tested:

    Connect (from both ends at once)
    CancelConnectionAttempt

Exercised indirectly by getting to that point: Startup,
SetMaximumIncomingConnections, Ping, Receive, DeallocatePacket, CloseConnection.
*/

using namespace RakNet;

namespace {

// 1234, not the 60000-60007 the rest of the suite binds. Harmless: the ctest
// RESOURCE_LOCK serialises every test regardless of which port it takes.
constexpr unsigned short kServerPort = 1234;

constexpr TimeMS kTestDurationMs = 10000;

// How long after the offline ping both sides aim to call Connect().
constexpr TimeMS kConnectDelayMs = 1000;

// How long after the simultaneous Connect() a notification must have arrived.
constexpr TimeMS kNotificationBudgetMs = 2000;

// Pause between tearing one round down and pinging for the next.
constexpr TimeMS kBetweenRoundsMs = 1000;

// How many rounds the budget above pays for. The cadence is all fixed delays
// over loopback - ping, +1000 to connect, +2000 to judge, +1000 to the next
// ping - so round two is judged at about 7 s against a 10 s budget, and the count
// is exactly 2 in every run measured. Asserted rather than merely counted: `> 0`
// would let a ping lost after round one idle the loop out and still pass.
constexpr int kExpectedRounds = 2;

} // namespace

// The absolute deadlines below go through ConnectionWaits::Expired, which is
// wrap-safe on a uint32_t TimeMS where a plain >= is not. (The
// `GetTimeMS() - entryTime < duration` loop below needs no such care: unsigned
// subtraction of two absolute times is already wrap-safe.)
using ConnectionWaits::Expired;

TEST_CASE( "Two peers connecting to each other at the same instant both get a connection notification", "[network]" )
{
    PeerScope peers;

    RakPeerInterface* server = peers.Server( kServerPort );
    RakPeerInterface* client = peers.Client();

    // Filled in from the offline ping, which is how the server learns where to
    // dial back. Zeroed, so a round that somehow fires before the ping lands dials
    // nowhere instead of reading a garbage stack buffer.
    char clientIp[128] = { 0 };
    unsigned short clientPort = 0;

    bool gotNotification = false;

    // Absolute times; 0 means "not scheduled".
    TimeMS connectionAttemptTime = 0;
    TimeMS notificationDeadline = 0;
    TimeMS nextRoundStartTime = 0;

    // One handler over both queues: in practice only the server sees the ping and
    // only the client sees the pong, but sharing it means the two cannot drift
    // apart. It drives the state machine above rather than logging progress.
    auto handle = [&]( const Packet* packet ) {
        switch( packet->data[0] )
        {
        case ID_NEW_INCOMING_CONNECTION:
        case ID_CONNECTION_REQUEST_ACCEPTED:
            gotNotification = true;
            break;

        case ID_UNCONNECTED_PING:
            packet->systemAddress.ToString( false, clientIp );
            clientPort = packet->systemAddress.GetPort();

            connectionAttemptTime = GetTimeMS() + kConnectDelayMs;
            gotNotification = false;
            break;

        case ID_UNCONNECTED_PONG: {
            TimeMS sendPingTime = 0;
            BitStream bs( packet->data, packet->length, false );
            bs.IgnoreBytes( 1 );
            bs.Read( sendPingTime );

            // Pull this side forward by the one-way trip so both Connect() calls
            // land together. A half-trip over half the delay means the estimate is
            // useless, so just go now.
            const TimeMS halfRoundTrip = ( GetTimeMS() - sendPingTime ) / 2;
            connectionAttemptTime = halfRoundTrip <= kConnectDelayMs / 2
                                        ? GetTimeMS() + kConnectDelayMs - halfRoundTrip
                                        : GetTimeMS();
            gotNotification = false;
            break;
        }

        default:
            break;
        }
    };

    // The two queues are drained identically; only the peer differs.
    auto drain = [&]( RakPeerInterface* peer ) {
        for( Packet* packet = peer->Receive(); packet; peer->DeallocatePacket( packet ), packet = peer->Receive() )
        {
            handle( packet );
        }
    };

    client->Ping( "127.0.0.1", kServerPort, false );

    int roundsChecked = 0;

    const TimeMS entryTime = GetTimeMS();
    while( GetTimeMS() - entryTime < kTestDurationMs )
    {
        drain( server );
        drain( client );

        if( connectionAttemptTime != 0 && Expired( connectionAttemptTime ) )
        {
            connectionAttemptTime = 0;

            server->Connect( clientIp, clientPort, 0, 0 );
            client->Connect( "127.0.0.1", kServerPort, 0, 0 );

            notificationDeadline = GetTimeMS() + kNotificationBudgetMs;
        }

        if( notificationDeadline != 0 && Expired( notificationDeadline ) )
        {
            notificationDeadline = 0;
            roundsChecked++;

            // CHECK, not FAIL: each round reports for itself, so a handshake that
            // breaks only on the third round is distinguishable from one that
            // breaks on every round.
            INFO( "cross-connection round " << roundsChecked );
            CHECK( gotNotification );

            client->CancelConnectionAttempt( SystemAddress( "127.0.0.1", kServerPort ) );
            server->CancelConnectionAttempt( SystemAddress( clientIp, clientPort ) );

            server->CloseConnection( server->GetSystemAddressFromIndex( 0 ), true, 0 );
            client->CloseConnection( client->GetSystemAddressFromIndex( 0 ), true, 0 );

            nextRoundStartTime = GetTimeMS() + kBetweenRoundsMs;
        }

        if( nextRoundStartTime != 0 && Expired( nextRoundStartTime ) )
        {
            nextRoundStartTime = 0;
            client->Ping( "127.0.0.1", kServerPort, false );
        }

        // A yield, not a pause, and kept at zero deliberately: the handshake this
        // test is about is timed in milliseconds off the pong, so the loop wants to
        // see a packet the moment it lands. Sleeping here would blunt what is being
        // measured.
        std::this_thread::sleep_for( std::chrono::milliseconds( 0 ) );
    }

    // Rules out the vacuous pass, where the loop idles ten seconds with two deaf
    // peers. `>= kExpectedRounds` rather than `> 0`: each ping is scheduled by the
    // round before it, so one lost after round one leaves the loop idling with a
    // single round banked - the same vacuous pass one step along.
    CHECK( roundsChecked >= kExpectedRounds );
}
