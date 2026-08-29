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
#include "RakNetSocket2.h"
#include "RakPeerInterface.h"
#include "RakTimer.h"
#include "TestHelpers.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>
#include <vector>

/*
Connects a client to a server the ordinary way, disconnects it, then reconnects it
twice more over a socket the test hands to RakNet itself - once with a socket taken
from GetSockets, once with one taken from GetSocket. Each reconnection is followed
by a send, because a connection that cannot carry a packet has not really been
re-established.

RakPeerInterface functions explicitly tested:

    ConnectWithSocket
    GetSockets
    GetSocket

Exercised indirectly by getting to that point: Startup,
SetMaximumIncomingConnections, Receive, DeallocatePacket, Send, IsConnected.
*/

using namespace RakNet;

namespace {

constexpr unsigned short kServerPort = 60000;

/*
 *  ConnectWithSocket is fire-and-forget, so this polls: retry whenever the client
 *  is not already connected or on its way, until connected or the budget runs out.
 *  Shared by both call sites so the test body reads as the three connections it is.
 */
bool ConnectWithSocketAndWait( RakPeerInterface* client, const SystemAddress& serverAddress, RakNetSocket2* socket, int millisecondsToWait )
{
    RakTimer timer( millisecondsToWait );

    while( !CommonFunctions::ConnectionStateMatchesOptions( client, serverAddress, true ) && !timer.IsExpired() )
    {
        if( !CommonFunctions::ConnectionStateMatchesOptions( client, serverAddress, true, true, true, true ) )
        {
            client->ConnectWithSocket( "127.0.0.1", serverAddress.GetPort(), 0, 0, socket );
        }

        std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
    }

    return CommonFunctions::ConnectionStateMatchesOptions( client, serverAddress, true );
}

} // namespace

TEST_CASE( "A client reconnects and sends over a socket it supplies, from both GetSockets and GetSocket", "[network]" )
{
    PeerScope peers;

    RakPeerInterface* client = peers.Client();
    RakPeerInterface* server = peers.Server( kServerPort );

    const SystemAddress serverAddress( "127.0.0.1", kServerPort );

    // Everything below reconnects this same pair, so an ordinary connect that
    // fails leaves nothing worth measuring.
    REQUIRE( TestHelpers::WaitAndConnectTwoPeersLocally( client, server, 5000 ) );

    // The control: prove a send arrives before ConnectWithSocket is in the
    // picture, so a later send failure is not blamed on the socket.
    TestHelpers::BroadCastTestPacket( client );
    CHECK( TestHelpers::WaitForTestPacket( server, 5000 ) );

    // Closed once and then waited for, never re-issued per poll - see
    // ConnectionWaits::WaitForDisconnect for why that distinction matters.
    client->CloseConnection( serverAddress, true, 0, LOW_PRIORITY );
    ConnectionWaits::WaitForDisconnect( client, serverAddress );

    std::vector<RakNetSocket2*> sockets;
    client->GetSockets( sockets );

    // REQUIRE, not CHECK: sockets[0] on an empty list is undefined behaviour
    // rather than a second failure message.
    REQUIRE_FALSE( sockets.empty() );

    REQUIRE( ConnectWithSocketAndWait( client, serverAddress, sockets[0], 5000 ) );

    TestHelpers::BroadCastTestPacket( client );
    CHECK( TestHelpers::WaitForTestPacket( server, 5000 ) );

    client->CloseConnection( serverAddress, true, 0, LOW_PRIORITY );
    ConnectionWaits::WaitForDisconnect( client, serverAddress );

    // The same socket by the other route. UNASSIGNED_SYSTEM_ADDRESS asks for the
    // client's own open socket rather than one bound to a peer.
    RakNetSocket2* openSocket = client->GetSocket( UNASSIGNED_SYSTEM_ADDRESS );
    REQUIRE( openSocket != nullptr );

    REQUIRE( ConnectWithSocketAndWait( client, serverAddress, openSocket, 5000 ) );

    TestHelpers::BroadCastTestPacket( client );
    CHECK( TestHelpers::WaitForTestPacket( server, 5000 ) );
}
