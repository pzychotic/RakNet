/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "ConnectionWaits.h"
#include "PeerScope.h"

#include "BitStream.h"
#include "GetTime.h"
#include "MessageIdentifiers.h"
#include "RakNetTime.h"
#include "RakNetTypes.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>
#include <vector>

/*
Eight peers connect to one another until every peer holds the other seven, then
each of them broadcasts a hundred RELIABLE_ORDERED packets carrying its own index
and a sequence number. Every peer must receive all hundred from each of the other
seven, in the order they were sent, and hold every connection for the duration.

RakPeerInterface functions explicitly tested:

    Send (HIGH_PRIORITY, RELIABLE_ORDERED, broadcast)
    GetSystemList

Exercised indirectly by getting to that point: Startup,
SetMaximumIncomingConnections, Connect, Receive, DeallocatePacket,
GetConnectionState.

Connectedness is read from GetSystemList rather than counted out of
ID_CONNECTION_REQUEST_ACCEPTED and ID_NEW_INCOMING_CONNECTION packets by hand.

The tail of the receive loop is where the runtime of a test like this hides: a
per-peer wait of up to a second for one more packet, on eight peers with empty
queues, cost 126.7 s in the baseline (docs/research/test-suite-baseline.md). The
drain below is bounded by kDeliveryBudget but ends as soon as the counts are
complete, and the test runs in about 2.6 s in Release - measured, which is why it
does not carry [slow].
*/

using namespace RakNet;

namespace {

constexpr int kPeerNum = 8;
constexpr unsigned short kBasePort = 60000;

// Deliberately not equal: room for twice the peer count in total, incoming capped
// at the peer count. Every peer needs the other seven, so neither number binds.
constexpr unsigned int kMaxConnections = kPeerNum * 2;
constexpr unsigned short kMaxIncoming = kPeerNum;

constexpr int kNumPackets = 100;
constexpr unsigned char kPacketId = ID_USER_PACKET_ENUM + 1;

// Hang guard on the tail of the send stream, not a tuning knob: RELIABLE_ORDERED
// promises every packet arrives, so this bounds a wedged run rather than buying
// delivery. Over loopback the drain below exits in well under a second.
constexpr TimeMS kDeliveryBudget = 30000;

} // namespace

// Wrap-safe on a uint32_t TimeMS, where a plain >= is not. See ConnectionWaits.h.
using ConnectionWaits::Expired;

TEST_CASE( "Eight fully connected peers each receive every packet the other seven broadcast, in order", "[network]" )
{
    PeerScope peers;

    RakPeerInterface* peerList[kPeerNum];

    for( int i = 0; i < kPeerNum; i++ )
    {
        // Client() rather than Server(), even though these peers accept
        // connections: Server() sets the incoming limit to its Startup slot
        // count, and this test wants those two numbers different.
        peerList[i] = peers.Client( static_cast<unsigned short>( kBasePort + i ), kMaxConnections );
        peerList[i]->SetMaximumIncomingConnections( kMaxIncoming );
    }

    for( int i = 0; i < kPeerNum; i++ )
    {
        // From i + 1, so a pair is attempted once rather than from both ends.
        for( int j = i + 1; j < kPeerNum; j++ )
        {
            INFO( "peer " << i << " connecting to peer " << j );
            REQUIRE( peerList[i]->Connect( "127.0.0.1", kBasePort + j, 0, 0 ) == CONNECTION_ATTEMPT_STARTED );
        }
    }

    // Every ordered pair, both directions of each attempt - see ConnectionWaits.h.
    // This waits for the requests to settle, not to succeed; the count below is
    // what says they succeeded.
    ConnectionWaits::WaitForAllPairsToSettle( peerList, kPeerNum, kBasePort );

    std::vector<SystemAddress> systemList;
    std::vector<RakNetGUID> guidList;

    for( int i = 0; i < kPeerNum; i++ )
    {
        peerList[i]->GetSystemList( systemList, guidList );

        const int connectionCount = static_cast<int>( guidList.size() );

        // REQUIRE: every send below is a broadcast to whoever is connected, so a
        // peer short of its seven silently turns the packet counts at the bottom
        // into a report about a connection failure that happened up here.
        INFO( "peer " << i );
        REQUIRE( connectionCount == kPeerNum - 1 );
    }

    // The connection notifications are of no further interest, and the send
    // stream below is easier to reason about against empty queues.
    ConnectionWaits::DrainAll( peerList, kPeerNum );

    // [receiver][sender]
    int receivedFrom[kPeerNum][kPeerNum] = {};
    int nextExpected[kPeerNum][kPeerNum] = {};

    auto handleReceived = [&]( int receiver, const Packet* packet ) {
        const unsigned char id = packet->data[0];

        // Every peer was connected a moment ago and nothing here closes a
        // connection, so any of these is the test's subject breaking.
        if( id == ID_DISCONNECTION_NOTIFICATION || id == ID_CONNECTION_LOST ||
            id == ID_REMOTE_DISCONNECTION_NOTIFICATION || id == ID_REMOTE_CONNECTION_LOST )
        {
            FAIL( "peer " << receiver << " lost a connection mid-test, message id " << static_cast<int>( id ) );
        }

        if( id != kPacketId )
        {
            return;
        }

        BitStream bitStream( packet->data, packet->length, false );
        bitStream.IgnoreBytes( 1 );

        int sequence = 0;
        int sender = 0;

        // REQUIRE: read unchecked, a short packet would index the two 8x8 tables
        // below with whatever the stack held.
        REQUIRE( bitStream.Read( sequence ) );
        REQUIRE( bitStream.Read( sender ) );

        REQUIRE( sender >= 0 );
        REQUIRE( sender < kPeerNum );

        INFO( "peer " << receiver << " receiving from peer " << sender );

        // REQUIRE rather than the suite's CHECK default, because this latches: the
        // expected number advances one per packet, so one break in the ordering
        // puts every packet behind it out of step too, and a CHECK would report
        // the same defect hundreds of times instead of reporting more.
        REQUIRE( sequence == nextExpected[receiver][sender] );

        nextExpected[receiver][sender]++;
        receivedFrom[receiver][sender]++;
    };

    // Deliberately NOT ConnectionWaits::DrainAll, and deliberately not named like
    // one: this does not throw the packets away, it feeds every one of them to the
    // state machine above, which is the test's subject.
    auto handleAllReceived = [&]() {
        for( int i = 0; i < kPeerNum; i++ )
        {
            for( Packet* packet = peerList[i]->Receive(); packet;
                 peerList[i]->DeallocatePacket( packet ), packet = peerList[i]->Receive() )
            {
                handleReceived( i, packet );
            }
        }
    };

    for( int sequence = 0; sequence < kNumPackets; sequence++ )
    {
        for( int i = 0; i < kPeerNum; i++ )
        {
            BitStream bitStream;
            bitStream.Write( kPacketId );
            bitStream.Write( sequence );
            bitStream.Write( i );

            // Send fails on an empty or null buffer rather than on a busy peer, so
            // a false here is a bug in the three lines above it.
            INFO( "peer " << i << " sending packet " << sequence );
            REQUIRE( peerList[i]->Send( &bitStream, HIGH_PRIORITY, RELIABLE_ORDERED, 0, UNASSIGNED_SYSTEM_ADDRESS, true ) );
        }

        handleAllReceived();

        // A yield rather than a pause.
        std::this_thread::sleep_for( std::chrono::milliseconds( 0 ) );
    }

    auto allDelivered = [&]() {
        for( int i = 0; i < kPeerNum; i++ )
        {
            for( int j = 0; j < kPeerNum; j++ )
            {
                if( i != j && receivedFrom[i][j] < kNumPackets )
                {
                    return false;
                }
            }
        }

        return true;
    };

    const TimeMS deliveryDeadline = GetTimeMS() + kDeliveryBudget;

    while( !allDelivered() && !Expired( deliveryDeadline ) )
    {
        handleAllReceived();

        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }

    // CHECK: these are the last statements in the test, and a peer that is short
    // of one sender's packets should not hide the other 55 pairs.
    for( int i = 0; i < kPeerNum; i++ )
    {
        for( int j = 0; j < kPeerNum; j++ )
        {
            if( i == j )
            {
                continue;
            }

            INFO( "peer " << i << " from peer " << j );
            CHECK( receivedFrom[i][j] == kNumPackets );
        }
    }
}
