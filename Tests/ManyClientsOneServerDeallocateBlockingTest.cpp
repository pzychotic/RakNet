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
#include "RakPeerInterface.h"
#include "RakNetTypes.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>
#include <vector>

/*
The server's timeout is set to one second, then 256 clients connect to it.

For thirty seconds, every client that has managed to connect is destroyed outright
- no CloseConnection, no disconnect notification - and a fresh one is started in
its place. The test sleeps for twice the timeout so the server's timeout is what
notices, then reconnects everyone. Deallocating a live client is the point: this is
the only test in the suite that does it, and it is what PeerScope::ReplaceWithClient
exists for.

After the loop, everyone is given one last chance to connect, and all 256 must be
holding exactly one connection.

This version waits for connects in a blocking loop, hence the name; its
non-blocking sibling is ManyClientsOneServerNonBlockingTest.

RakPeerInterface functions explicitly tested:

    SetTimeoutTime
    GetTimeoutTime
    Connect
    GetSystemList

Exercised indirectly by getting to that point: Startup,
SetMaximumIncomingConnections, Receive, DeallocatePacket, GetConnectionState.
*/

using namespace RakNet;

namespace {

constexpr unsigned short kServerPort = 60000;
constexpr int kClientNum = 256;
constexpr TimeMS kTimeoutTime = 1000;

void WaitAndDrain( RakPeerInterface* const* clientList, RakPeerInterface* server )
{
    const SystemAddress serverAddress( "127.0.0.1", kServerPort );

    ConnectionWaits::WaitForRequestsToSettle( clientList, kClientNum, serverAddress );

    ConnectionWaits::Drain( server );
    ConnectionWaits::DrainAll( clientList, kClientNum );
}

} // namespace

TEST_CASE( "Clients deallocated mid-connection are timed out by the server and all 256 reconnect", "[network][slow]" )
{
    PeerScope peers;

    // The server first: creation order is teardown order reversed, and destroying
    // 256 connected clients before the server they are connected to is the order
    // PeerScope is written around.
    RakPeerInterface* server = peers.Server( kServerPort, kClientNum );

    server->SetTimeoutTime( kTimeoutTime, UNASSIGNED_SYSTEM_ADDRESS );

    // REQUIRE: the thirty-second loop below is built entirely on the server
    // noticing a vanished client within this window, so a timeout that did not
    // take makes everything after it meaningless.
    REQUIRE( server->GetTimeoutTime( UNASSIGNED_SYSTEM_ADDRESS ) == kTimeoutTime );

    RakPeerInterface* clientList[kClientNum];

    for( int i = 0; i < kClientNum; i++ )
    {
        clientList[i] = peers.Client();
    }

    for( int i = 0; i < kClientNum; i++ )
    {
        INFO( "client " << i );
        REQUIRE( clientList[i]->Connect( "127.0.0.1", kServerPort, 0, 0 ) == CONNECTION_ATTEMPT_STARTED );
    }

    const SystemAddress serverAddress( "127.0.0.1", kServerPort );
    std::vector<SystemAddress> systemList;
    std::vector<RakNetGUID> guidList;

    const TimeMS entryTime = GetTimeMS();

    while( GetTimeMS() - entryTime < 30000 )
    {
        for( int i = 0; i < kClientNum; i++ )
        {
            clientList[i]->GetSystemList( systemList, guidList );

            if( !systemList.empty() )
            {
                INFO( "client " << i );
                peers.ReplaceWithClient( clientList[i] );
            }
        }

        std::this_thread::sleep_for( std::chrono::milliseconds( 2000 ) ); //Allow connections to timeout.

        for( int i = 0; i < kClientNum; i++ )
        {
            if( !CommonFunctions::ConnectionStateMatchesOptions( clientList[i], serverAddress, true, true, true, true ) )
            {
                INFO( "client " << i );
                REQUIRE( clientList[i]->Connect( "127.0.0.1", kServerPort, 0, 0 ) == CONNECTION_ATTEMPT_STARTED );
            }
        }

        WaitAndDrain( clientList, server );
    }

    WaitAndDrain( clientList, server );

    std::this_thread::sleep_for( std::chrono::milliseconds( 2000 ) ); //Allow connections to timeout.

    for( int i = 0; i < kClientNum; i++ )
    {
        if( !CommonFunctions::ConnectionStateMatchesOptions( clientList[i], serverAddress, true, true, true, true ) )
        {
            INFO( "client " << i );
            REQUIRE( clientList[i]->Connect( "127.0.0.1", kServerPort, 0, 0 ) == CONNECTION_ATTEMPT_STARTED );
        }
    }

    WaitAndDrain( clientList, server );

    // CHECK, not REQUIRE: this is the last statement in the test, so a client that
    // failed to reconnect costs nothing to keep going past, and reporting all of
    // them beats reporting the lowest-numbered one.
    for( int i = 0; i < kClientNum; i++ )
    {
        clientList[i]->GetSystemList( systemList, guidList );

        INFO( "client " << i );
        CHECK( guidList.size() == 1 );
    }
}
