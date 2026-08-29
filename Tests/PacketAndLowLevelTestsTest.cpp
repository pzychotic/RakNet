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
#include "TestHelpers.h"

#include <catch2/catch_test_macros.hpp>

/*
Walks a connected pair through the low-level packet API - a batched SendList, a
packet large enough to force splitting and report ID_DOWNLOAD_PROGRESS, and two
packets injected straight into the receive queue with PushBackPacket - checking
after each that ordinary send and receive still work. That "still works" check is
the real subject: these calls reach past the normal send path, and the risk is that
they leave the queue in a state later traffic cannot get out of.

RakPeerInterface functions explicitly tested:

    SendList
    SetSplitMessageProgressInterval
    GetSplitMessageProgressInterval
    AllocatePacket
    PushBackPacket

Exercised indirectly by getting to that point: Startup,
SetMaximumIncomingConnections, Receive, DeallocatePacket, Send, IsConnected.

AttachPlugin, DetachPlugin and GetMTUSize are NOT covered, despite the name this
test used to carry: the plugin half was never more than a commented-out block and
GetMTUSize was never called at all.
*/

using namespace RakNet;

namespace {

constexpr unsigned short kServerPort = 60000;

// Large enough to be split by any plausible MTU, which is what makes
// ID_DOWNLOAD_PROGRESS fire.
constexpr int kHugePacketSize = 3000000;

// Five messages of "AAAA" plus terminator. SendList coalesces them into one
// packet, so the receiver sees 5 x 5 bytes in one delivery.
constexpr int kSendListMessages = 5;
constexpr int kSendListMessageLen = 5;

} // namespace

TEST_CASE( "SendList, split packets and PushBackPacket deliver without breaking later traffic", "[network]" )
{
    PeerScope peers;

    RakPeerInterface* client = peers.Client();
    RakPeerInterface* server = peers.Server( kServerPort );

    REQUIRE( TestHelpers::WaitAndConnectTwoPeersLocally( client, server, 5000 ) );

    char curString1[] = "AAAA";
    char curString2[] = "ABBB";
    char curString3[] = "ACCC";
    char curString4[] = "ADDD";
    char curString5[] = "AEEE";

    char* dataList[kSendListMessages] = { curString1, curString2, curString3, curString4, curString5 };
    int lengths[kSendListMessages];

    for( int i = 0; i < kSendListMessages; i++ )
    {
        dataList[i][0] = ID_USER_PACKET_ENUM + 1 + i;
        lengths[i] = kSendListMessageLen;
    }

    client->SendList( (const char**)dataList, lengths, kSendListMessages, HIGH_PRIORITY, RELIABLE_ORDERED, 0, UNASSIGNED_SYSTEM_ADDRESS, true );

    Packet* packet = CommonFunctions::WaitAndReturnMessageWithID( server, ID_USER_PACKET_ENUM + 1, 1000 );

    // REQUIRE: packet->length below dereferences it.
    REQUIRE( packet != nullptr );
    CHECK( packet->length == kSendListMessages * kSendListMessageLen );

    server->DeallocatePacket( packet );

    TestHelpers::BroadCastTestPacket( client );
    CHECK( TestHelpers::WaitForTestPacket( server, 2000 ) );

    // Both are handed to PushBackPacket further down, which is what eventually
    // frees them.
    Packet* hugePacket = client->AllocatePacket( kHugePacketSize );
    REQUIRE( hugePacket != nullptr );

    Packet* hugePacket2 = client->AllocatePacket( kHugePacketSize );
    REQUIRE( hugePacket2 != nullptr );

    hugePacket->data[0] = ID_USER_PACKET_ENUM + 1;
    hugePacket2->data[0] = ID_USER_PACKET_ENUM + 1;

    // One millisecond, so progress is reported as often as it possibly can be.
    server->SetSplitMessageProgressInterval( 1 );
    CHECK( server->GetSplitMessageProgressInterval() == 1 );

    // REQUIRE: with nothing sent there is no split to report progress on.
    REQUIRE( client->Send( (const char*)hugePacket->data, kHugePacketSize, HIGH_PRIORITY, RELIABLE_ORDERED, 0, UNASSIGNED_SYSTEM_ADDRESS, true ) > 0 );

    CHECK( CommonFunctions::WaitForMessageWithID( server, ID_DOWNLOAD_PROGRESS, 2000 ) );

    // Drain the rest of the progress reports, so the checks below are looking at
    // their own traffic.
    while( CommonFunctions::WaitForMessageWithID( server, ID_DOWNLOAD_PROGRESS, 500 ) )
    {
    }

    // A three-megabyte transfer can cost the connection; reconnect if so, since
    // every check below needs the pair connected. Returns true immediately when
    // the connection survived.
    REQUIRE( TestHelpers::WaitAndConnectTwoPeersLocally( client, server, 5000 ) );

    TestHelpers::BroadCastTestPacket( client );
    CHECK( TestHelpers::WaitForTestPacket( server, 5000 ) );

    // Onto the tail of the receive queue.
    server->PushBackPacket( hugePacket, false );
    CHECK( TestHelpers::WaitForTestPacket( server, 2000 ) );

    REQUIRE( TestHelpers::WaitAndConnectTwoPeersLocally( client, server, 5000 ) );

    TestHelpers::BroadCastTestPacket( client );
    CHECK( TestHelpers::WaitForTestPacket( server, 2000 ) );

    // And onto the head of it.
    server->PushBackPacket( hugePacket2, true );
    CHECK( TestHelpers::WaitForTestPacket( server, 2000 ) );

    REQUIRE( TestHelpers::WaitAndConnectTwoPeersLocally( client, server, 5000 ) );

    TestHelpers::BroadCastTestPacket( client );
    CHECK( TestHelpers::WaitForTestPacket( server, 2000 ) );
}
