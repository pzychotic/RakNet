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

#include "MessageIdentifiers.h"
#include "Rand.h"
#include "RakNetTime.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>
#include <vector>

/*
Nine clients spend 42 rounds, about 17 seconds, randomly closing their connection
to a server - sometimes with a disconnection notification, sometimes silently, so
that only the server's one-second timeout can notice - and reconnecting
afterwards. Fifteen of the rounds are wait rounds, each judged by a drop round at
the top of the next. Two things are asserted throughout:

  - The server never holds two connections to one client: no two of its connected
    Systems share a RakNetGUID or an address. That is what a reconnect accepted
    before the server has cleaned up the old connection would look like.
  - After a settle window with no connects or closes in it, the number of clients
    that count the server matches the number of clients the server counts. That is
    the timeout detection itself: a client that closed silently is gone on its own
    side immediately and only leaves the server's list when the timeout fires.

RakPeerInterface functions explicitly tested:

    SetTimeoutTime
    CloseConnection (with and without a disconnection notification)
    GetConnectionList
    GetSystemList
    IsActive

Exercised indirectly by getting to that point: Startup,
SetMaximumIncomingConnections, Connect, GetConnectionState (through
CommonFunctions::ConnectionStateMatchesOptions), Receive, DeallocatePacket.

The randomness and its fixed seed are both load-bearing. The interleaving of
silent closes, notified closes and reconnects is the coverage here - a fixed
script would test one ordering of it - and the seed is what makes a red run
reproducible rather than re-measurable.

Nothing in Source/ draws from the global Mersenne Twister in this test's process:
the only two sites (in ReliabilityLayer::SendBitStream) are _DEBUG-only and gated on
ApplyNetworkSimulator settings this test never applies, so the seed really does
determine every draw. What it does not fix is the schedule - packet timing and the
loop cadence still vary - so a replay repeats the same sequence of actions, not the
same milliseconds. The loop is bounded by its round count rather than by wall time
for the same reason, so the seed also fixes how many of those rounds are drop
rounds, and the test asserts that number exactly.
*/

using namespace RakNet;

namespace {

constexpr int kNumberOfClients = 9;

// 20000 for the server, 20001-20009 for the clients - not the 30000s most of the
// suite binds. Harmless: the ctest RESOURCE_LOCK is one global lock and
// serialises every test whichever port it takes.
constexpr unsigned short kServerPort = 20000;
constexpr unsigned short kFirstClientPort = kServerPort + 1;

// The asymmetry is the point: a silently closed connection disappears from the
// client at once and has to age out of the server.
constexpr TimeMS kServerTimeoutMs = 1000;
constexpr TimeMS kClientTimeoutMs = 5000;

// The last of the 42 rounds is the drop round for the fifteenth wait. The seed
// fixes both numbers, so a change to its sequence fails the count at the end
// rather than quietly thinning the test out.
constexpr int kRounds = 42;
constexpr int kExpectedDropRounds = 15;

// Half the server's timeout, waited before the receive and again after it, so a
// drop round ages out the connections that were actually dropped without idling
// long enough to time out the ones that were not.
constexpr TimeMS kHalfTimeoutWaitMs = kServerTimeoutMs / 2;

// Keeps the loop off the CPU between rounds.
constexpr TimeMS kLoopSleepMs = 10;

// Keeps the execution path the same from run to run.
constexpr unsigned int kSeed = 12345;

} // namespace

TEST_CASE( "A server times out clients that close silently, and never holds two connections to one client when they reconnect", "[network]" )
{
    PeerScope peers;

    RakPeerInterface* server = peers.Server( kServerPort, kNumberOfClients );
    server->SetTimeoutTime( kServerTimeoutMs, UNASSIGNED_SYSTEM_ADDRESS );

    const SystemAddress serverAddress( "127.0.0.1", kServerPort );

    RakPeerInterface* clients[kNumberOfClients];

    for( int i = 0; i < kNumberOfClients; i++ )
    {
        clients[i] = peers.Client( static_cast<unsigned short>( kFirstClientPort + i ) );
        clients[i]->SetTimeoutTime( kClientTimeoutMs, UNASSIGNED_SYSTEM_ADDRESS );

        INFO( "client " << i );
        REQUIRE( clients[i]->Connect( "127.0.0.1", kServerPort, 0, 0 ) == CONNECTION_ATTEMPT_STARTED );
    }

    // Settled, then asserted *connected* separately: the settle wait is satisfied
    // by a failed request too (see ConnectionWaits.h), and everything below
    // assumes nine live connections.
    ConnectionWaits::WaitForRequestsToSettle( clients, kNumberOfClients, serverAddress );

    for( int i = 0; i < kNumberOfClients; i++ )
    {
        INFO( "client " << i );
        REQUIRE( CommonFunctions::ConnectionStateMatchesOptions( clients[i], serverAddress, true ) );
    }

    seedMT( kSeed );

    // Reads the server's connected Systems and asserts that no two of them are the
    // same client, by RakNetGUID or by address. Each client is one Peer on its own
    // port, so either match means the server accepted a reconnect while it still
    // held the old connection.
    //
    // REQUIRE rather than the suite's CHECK default: this runs every round, so a
    // CHECK would report the same defect once per round, and two connections to
    // one client are already a complete diagnosis.
    auto requireServerHoldsOneConnectionPerClient = [&]() {
        std::vector<SystemAddress> addresses;
        std::vector<RakNetGUID> guids;
        server->GetSystemList( addresses, guids );

        for( size_t i = 0; i < guids.size(); i++ )
        {
            for( size_t j = i + 1; j < guids.size(); j++ )
            {
                INFO( "connections to " << addresses[i].ToString( true ) << " and " << addresses[j].ToString( true ) );
                REQUIRE_FALSE( guids[i] == guids[j] );
                REQUIRE_FALSE( addresses[i] == addresses[j] );
            }
        }
    };

    // How many clients still count the server.
    auto connectedClientCount = [&]() {
        unsigned short connected = 0;

        for( int i = 0; i < kNumberOfClients; i++ )
        {
            unsigned short connections = 0;
            clients[i]->GetConnectionList( 0, &connections );

            if( connections != 0 )
            {
                connected++;
            }
        }

        return connected;
    };

    // Set by the wait round, read by the round after it.
    bool dropTestPending = false;

    int dropRounds = 0;

    for( int round = 1; round <= kRounds; round++ )
    {
        INFO( "round " << round );

        // Drawn before the check below, so the sequence of actions is the one the
        // seed describes whichever branch the check takes.
        const unsigned int randomTest = randomMT() % 4;

        if( dropTestPending )
        {
            dropTestPending = false;
            dropRounds++;

            unsigned short serverConnections = 0;
            server->GetConnectionList( 0, &serverConnections );

            const unsigned short clientsConnected = connectedClientCount();

            // The timeout detection itself. Both halves of the server's timeout
            // have passed since the last connect or close, so every connection
            // the server still counts must be one a client still counts too.
            //
            // CHECK, not REQUIRE: this fires once per drop round, and a run
            // where round one passes and round four fails says something a run
            // that stops at round four does not.
            INFO( "drop round " << dropRounds );
            CHECK( clientsConnected == serverConnections );
        }

        switch( randomTest )
        {
        case 0:
            // Close one client silently, so only the server's timeout can notice.
            clients[randomMT() % kNumberOfClients]->CloseConnection( serverAddress, false, 0 );
            break;

        case 1: {
            RakPeerInterface* client = clients[randomMT() % kNumberOfClients];

            // Only if there is nothing already connected or in flight - a Connect()
            // on a live connection returns ALREADY_CONNECTED and queues nothing.
            if( !CommonFunctions::ConnectionStateMatchesOptions( client, serverAddress, true, true, true, true ) )
            {
                REQUIRE( client->Connect( "127.0.0.1", kServerPort, 0, 0 ) == CONNECTION_ATTEMPT_STARTED );
            }
            break;
        }

        case 2:
            // Every client at once: half of them close, half of them reconnect.
            for( int i = 0; i < kNumberOfClients; i++ )
            {
                if( randomMT() % 2 == 0 )
                {
                    if( clients[i]->IsActive() )
                    {
                        const bool sendDisconnectionNotification = randomMT() % 2 == 0;
                        clients[i]->CloseConnection( serverAddress, sendDisconnectionNotification, 0 );
                    }
                }
                else if( !CommonFunctions::ConnectionStateMatchesOptions( clients[i], serverAddress, true, true, true, true ) )
                {
                    INFO( "client " << i );
                    REQUIRE( clients[i]->Connect( "127.0.0.1", kServerPort, 0, 0 ) == CONNECTION_ATTEMPT_STARTED );
                }
            }
            break;

        case 3:
            // Do nothing this round but let the clock run, and judge the result at
            // the top of the next one.
            std::this_thread::sleep_for( std::chrono::milliseconds( kHalfTimeoutWaitMs ) );
            dropTestPending = true;
            break;

        default:
            break;
        }

        ConnectionWaits::Drain( server );
        ConnectionWaits::DrainAll( clients, kNumberOfClients );

        // After the drains, so the handshakes this round's connects started have
        // had the drain's time to finish.
        requireServerHoldsOneConnectionPerClient();

        if( dropTestPending )
        {
            // The other half of the timeout, spent after the receive above rather
            // than before it so the drain cannot hide a connection that was about
            // to age out.
            std::this_thread::sleep_for( std::chrono::milliseconds( kHalfTimeoutWaitMs ) );
        }

        std::this_thread::sleep_for( std::chrono::milliseconds( kLoopSleepMs ) );
    }

    // Asserted rather than merely counted because the drop check above is the
    // whole point of the test: with fewer of them, the rounds above thin out into
    // a connect-and-close exercise that asks less often whether a timeout was
    // detected.
    CHECK( dropRounds == kExpectedDropRounds );
}
