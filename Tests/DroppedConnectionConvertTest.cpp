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
#include "Rand.h"
#include "RakNetTime.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

/*
Nine clients spend thirty seconds randomly closing their connection to a server -
sometimes with a disconnection notification, sometimes silently, so that only the
server's two-second timeout can notice - and reconnecting afterwards. Two things
are asserted throughout:

  - No client, started for one connection, ever holds two. That is what a
    reconnect onto a connection the server has not yet cleaned up would look like.
  - After a settle window with no connects or closes in it, the number of clients
    that count the server matches the number of clients the server counts. That is
    the timeout detection itself: a client that closed silently is gone on its own
    side immediately and only leaves the server's list when the timeout fires.

RakPeerInterface functions explicitly tested:

    SetTimeoutTime
    CloseConnection (with and without a disconnection notification)
    GetConnectionList
    IsActive

Exercised indirectly by getting to that point: Startup,
SetMaximumIncomingConnections, Connect, GetConnectionState (through
CommonFunctions::ConnectionStateMatchesOptions), Receive, DeallocatePacket.

The randomness and its fixed seed are both load-bearing. The interleaving of
silent closes, notified closes and reconnects is the coverage here - a fixed
script would test one ordering of it - and the seed is what makes a red run
reproducible rather than re-measurable.

Nothing in Source/ draws from the global Mersenne Twister in this test's process:
the only two sites (ReliabilityLayer.cpp:1887,1905) are _DEBUG-only and gated on
ApplyNetworkSimulator settings this test never applies, so the seed really does
determine every draw. What it does not fix is the schedule - packet timing and the
loop cadence still vary - so a replay repeats the same sequence of actions, not the
same milliseconds.
*/

using namespace RakNet;

namespace {

constexpr int kNumberOfClients = 9;

// 20000 for the server, 20001-20009 for the clients - not the 60000s most of the
// suite binds. Harmless: the ctest RESOURCE_LOCK is one global lock and
// serialises every test whichever port it takes.
constexpr unsigned short kServerPort = 20000;
constexpr unsigned short kFirstClientPort = kServerPort + 1;

// The asymmetry is the point: a silently closed connection disappears from the
// client at once and has to age out of the server.
constexpr TimeMS kServerTimeoutMs = 2000;
constexpr TimeMS kClientTimeoutMs = 5000;

constexpr TimeMS kTestDurationMs = 30000;

// Half the server's timeout, waited before the receive and again after it, so a
// drop round ages out the connections that were actually dropped without idling
// long enough to time out the ones that were not.
constexpr TimeMS kHalfTimeoutWaitMs = kServerTimeoutMs / 2;

// Keeps the loop off the CPU between rounds.
constexpr TimeMS kLoopSleepMs = 10;

// Keeps the execution path the same from run to run.
constexpr unsigned int kSeed = 12345;

} // namespace

TEST_CASE( "A server times out clients that close silently, and they reconnect without ever holding two connections", "[network]" )
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

    // Reads every client's connection count and asserts the invariant on it: each
    // client is started for a single connection, so one holding two has
    // reconnected onto a connection the server had not finished cleaning up.
    //
    // REQUIRE rather than the suite's CHECK default: this runs every round of a
    // thirty-second loop, so a CHECK would report the same defect a few thousand
    // times, and a client started for one connection that holds two is already a
    // complete diagnosis.
    auto requireNoClientHoldsTwoConnections = [&]() {
        for( int i = 0; i < kNumberOfClients; i++ )
        {
            unsigned short connections = 0;
            clients[i]->GetConnectionList( 0, &connections );

            INFO( "client " << i );
            REQUIRE( connections <= 1 );
        }
    };

    // How many clients still count the server. A plain query, split from the
    // assertion above rather than folded into it: the one caller wants this
    // inside a CHECK, and Catch2 evaluates a CHECK's expression inside a
    // catch(...), which would swallow the REQUIRE's throw and turn a terminal
    // invariant breach into a confusing unexpected-exception report.
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

    const TimeMS entryTime = GetTimeMS();

    while( GetTimeMS() - entryTime < kTestDurationMs )
    {
        // Drawn before the check below, so the sequence of actions is the one the
        // seed describes whichever branch the check takes.
        const unsigned int randomTest = randomMT() % 4;

        if( dropTestPending )
        {
            dropTestPending = false;
            dropRounds++;

            unsigned short serverConnections = 0;
            server->GetConnectionList( 0, &serverConnections );

            // Both counts read before the CHECK, not inside it: see
            // connectedClientCount above for why nothing that can throw belongs
            // in a CHECK's expression.
            const unsigned short clientsConnected = connectedClientCount();

            // The timeout detection itself. Both halves of the server's timeout
            // have passed since the last connect or close, so every connection
            // the server still counts must be one a client still counts too.
            //
            // CHECK, not REQUIRE: this fires once per drop round, a handful of
            // times in thirty seconds, and a run where round one passes and round
            // four fails says something a run that stops at round four does not.
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

        // Now that this round's connects and closes have been issued.
        requireNoClientHoldsTwoConnections();

        ConnectionWaits::Drain( server );
        ConnectionWaits::DrainAll( clients, kNumberOfClients );

        if( dropTestPending )
        {
            // The other half of the timeout, spent after the receive above rather
            // than before it so the drain cannot hide a connection that was about
            // to age out.
            std::this_thread::sleep_for( std::chrono::milliseconds( kHalfTimeoutWaitMs ) );
        }

        std::this_thread::sleep_for( std::chrono::milliseconds( kLoopSleepMs ) );
    }

    // One draw in four is the wait round, and each costs about two seconds of the
    // thirty, so the count lands well into double figures. Asserted rather than
    // merely counted because the drop check above is the whole point of the test:
    // with none of them, everything above is a thirty-second connect-and-close
    // exercise that never asks whether a timeout was detected.
    CHECK( dropRounds > 0 );
}
