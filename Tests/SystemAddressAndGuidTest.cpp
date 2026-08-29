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
#include "RakNetStringMakers.h"
#include "TestHelpers.h"

#include "RakPeerInterface.h"
#include "RakNetTypes.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

/*
Connects one client to one server over loopback, then asks the client to describe
that single connection through every accessor RakPeerInterface offers. All of them
are looking at the same connection, so all of them must agree.

RakPeerInterface functions explicitly tested:

    NumberOfConnections
    GetSystemList
    IsActive
    GetSystemAddressFromIndex
    GetSystemAddressFromGuid
    GetGuidFromSystemAddress
    GetGUIDFromIndex
    GetExternalID

Exercised indirectly by getting to that point: Startup,
SetMaximumIncomingConnections, Receive, DeallocatePacket, Send, IsConnected.
*/

using namespace RakNet;

namespace {

constexpr unsigned short kServerPort = 60000;
constexpr unsigned short kClientPort = 60001;

} // namespace

TEST_CASE( "A connected client describes its server identically through every address and guid accessor", "[network]" )
{
    PeerScope peers;

    // Create() rather than Client(), because IsActive() before Startup is itself
    // one of the assertions: false on a fresh instance, true once started.
    RakPeerInterface* client = peers.Create();
    REQUIRE_FALSE( client->IsActive() );

    SocketDescriptor clientSocket( kClientPort, 0 );
    REQUIRE( client->Startup( 1, &clientSocket, 1 ) == RAKNET_STARTED );
    REQUIRE( client->IsActive() );

    RakPeerInterface* server = peers.Server( kServerPort );

    REQUIRE( TestHelpers::WaitAndConnectTwoPeersLocally( client, server, 5000 ) );

    std::vector<SystemAddress> systemList;
    std::vector<RakNetGUID> guidList;
    client->GetSystemList( systemList, guidList );

    // REQUIRE, not CHECK: both lists are indexed below, so a wrong size here is
    // undefined behaviour rather than a second failure message.
    REQUIRE( systemList.size() == guidList.size() );
    REQUIRE( systemList.size() == 1 );

    // CHECK: nothing below reads it.
    CHECK( client->NumberOfConnections() == 1 );

    const SystemAddress serverAddress( "127.0.0.1", kServerPort );
    const SystemAddress clientAddress( "127.0.0.1", kClientPort );
    const RakNetGUID serverGuid = server->GetGuidFromSystemAddress( UNASSIGNED_SYSTEM_ADDRESS );

    // Independent views of the same connection: CHECK, so one broken accessor
    // reports its siblings in the same run instead of hiding them.
    CHECK( systemList[0] == serverAddress );
    CHECK( guidList[0] == serverGuid );
    CHECK( client->GetSystemAddressFromIndex( 0 ) == serverAddress );
    CHECK( client->GetSystemAddressFromGuid( serverGuid ) == serverAddress );
    CHECK( client->GetGuidFromSystemAddress( serverAddress ) == serverGuid );
    CHECK( client->GetGUIDFromIndex( 0 ) == serverGuid );

    // Weak over loopback, where the external address is the internal one. A real
    // check of GetExternalID needs a test that leaves the machine.
    CHECK( client->GetExternalID( serverAddress ) == clientAddress );
}
