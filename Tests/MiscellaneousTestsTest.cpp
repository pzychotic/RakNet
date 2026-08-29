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

/*
An unconnected client advertises itself to a server, which must surface the
advertisement as ID_ADVERTISE_SYSTEM. No connection is established first - that is
the point of AdvertiseSystem.

RakPeerInterface functions explicitly tested:

    AdvertiseSystem

Exercised indirectly by getting to that point: Startup,
SetMaximumIncomingConnections, Receive, DeallocatePacket.

SetRouterInterface and RemoveRouterInterface are NOT covered, despite the name
this test used to carry: no router code was ever written for it.
*/

using namespace RakNet;

namespace {

constexpr unsigned short kServerPort = 60000;

} // namespace

TEST_CASE( "An unconnected client's AdvertiseSystem reaches a server as ID_ADVERTISE_SYSTEM", "[network]" )
{
    PeerScope peers;

    RakPeerInterface* client = peers.Client();
    RakPeerInterface* server = peers.Server( kServerPort );

    client->AdvertiseSystem( "127.0.0.1", kServerPort, 0, 0 );

    CHECK( CommonFunctions::WaitForMessageWithID( server, ID_ADVERTISE_SYSTEM, 5000 ) );
}
