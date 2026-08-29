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
#include "MessageIdentifiers.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstring>
#include <thread>

/*
Everything a peer can tell you about itself and its own connection, checked against
what it is actually doing: GetConnectionState is read at each step of a connect,
close and reconnect cycle, and the three local-identity calls are checked against
IsLocalIP.

RakPeerInterface functions explicitly tested:

    GetConnectionState (through CommonFunctions::ConnectionStateMatchesOptions)
    IsLocalIP
    SendLoopback
    GetLocalIP
    GetInternalID

Exercised indirectly by getting to that point: Startup,
SetMaximumIncomingConnections, Connect, CloseConnection, Receive,
DeallocatePacket.
*/

using namespace RakNet;

namespace {

constexpr unsigned short kServerPort = 60000;

constexpr int kLoopbackMessageId = ID_USER_PACKET_ENUM + 1;

} // namespace

TEST_CASE( "GetConnectionState follows a client through connect, close and reconnect, and SendLoopback, GetLocalIP and GetInternalID all stay local", "[network]" )
{
    PeerScope peers;

    RakPeerInterface* client = peers.Client();
    RakPeerInterface* server = peers.Server( kServerPort );

    const SystemAddress serverAddress( "127.0.0.1", kServerPort );

    // The Connect is asserted separately from the wait below, which discards
    // Connect()'s result: only this line distinguishes a refused request from a
    // slow one.
    REQUIRE( client->Connect( "127.0.0.1", kServerPort, 0, 0 ) == CONNECTION_ATTEMPT_STARTED );
    REQUIRE( CommonFunctions::WaitAndConnect( client, "127.0.0.1", kServerPort, 5000 ) );

    client->CloseConnection( serverAddress, true, 0, LOW_PRIORITY );

    // IS_CONNECTED is still an accepted answer: CloseConnection is asynchronous, so
    // the state need not have moved yet. What must not have happened is a jump
    // straight to IS_NOT_CONNECTED.
    CHECK( CommonFunctions::ConnectionStateMatchesOptions( client, serverAddress, true, false, false, true ) );

    // Long enough for the close to finish, so the Connect() below is a fresh
    // attempt rather than a no-op on a connection that is still up.
    std::this_thread::sleep_for( std::chrono::milliseconds( 1000 ) );

    // REQUIRE on the result, not a bare call: the check below accepts IS_CONNECTED
    // as well as connecting and pending, so if the close above had not taken effect
    // this Connect() would return ALREADY_CONNECTED, queue nothing, and "did it
    // detect a connecting client" would pass on the stale connection.
    REQUIRE( client->Connect( "127.0.0.1", kServerPort, 0, 0 ) == CONNECTION_ATTEMPT_STARTED );

    CHECK( CommonFunctions::ConnectionStateMatchesOptions( client, serverAddress, true, true, true ) );

    // REQUIRE: SendLoopback below is only interesting on a peer with a live
    // connection to ignore.
    REQUIRE( CommonFunctions::WaitAndConnect( client, "127.0.0.1", kServerPort, 5000 ) );

    CHECK( client->IsLocalIP( "127.0.0.1" ) );

    // One copy is enough: SendLoopback pushes straight onto the peer's own
    // receive queue (RakPeer.cpp), so nothing here can drop it in transit.
    char loopbackMessage[] = "AAAAAAAAAA";
    loopbackMessage[0] = static_cast<char>( kLoopbackMessageId );
    client->SendLoopback( loopbackMessage, static_cast<int>( strlen( loopbackMessage ) ) + 1 );

    CHECK( CommonFunctions::WaitForMessageWithID( client, kLoopbackMessageId, 1000 ) );

    const char* localIp = client->GetLocalIP( 0 );

    // REQUIRE: IsLocalIP reads through it.
    REQUIRE( localIp != nullptr );
    CHECK( client->IsLocalIP( localIp ) );

    char internalIp[128] = { 0 };
    client->GetInternalID().ToString( false, internalIp );

    INFO( "GetInternalID returned " << internalIp );
    CHECK( client->IsLocalIP( internalIp ) );
}
