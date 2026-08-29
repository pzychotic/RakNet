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
#include "Rand.h"
#include "RakNetTime.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

/*
A sender pushes one RELIABLE_ORDERED packet every 30 ms for twelve seconds, on a
single channel, each with a randomly sized payload so the reliability layer has to
split and reassemble rather than move one fixed size. Every packet carries its own
sequence number, and the receiver checks that the numbers arrive consecutively - so
a drop, a duplicate or a reordering is caught at the packet it happens on.

RakPeerInterface functions explicitly tested:

    Send (HIGH_PRIORITY, RELIABLE_ORDERED)

Exercised indirectly by getting to that point: Startup,
SetMaximumIncomingConnections, Connect, Receive, DeallocatePacket.

SetMalloc_Ex, SetRealloc_Ex and SetFree_Ex are NOT covered, despite a set of
allocation hooks this test used to carry: they were inside a comment block and
their logging functions were non-static members that could not have been passed to
the setters anyway.
*/

using namespace RakNet;

namespace {

constexpr unsigned short kReceiverPort = 60000;

// Only one connection is ever made; the number is what the test was measured at.
constexpr unsigned int kReceiverMaxConnections = 32;

// One channel, one constant. It stays on the wire as one more field whose round
// trip is checked - never as an index into anything, since a malformed packet
// could carry any of 256 values.
constexpr unsigned char kChannel = 0;

constexpr unsigned char kPacketId = ID_USER_PACKET_ENUM + 1;

constexpr TimeMS kTestDurationMs = 12000;
constexpr TimeMS kSendIntervalMs = 30;

// Payload size is 1 to kMaxPadBytes, drawn from the global Mersenne Twister.
// Random on purpose: varying the size across the split threshold is the only
// reason this test sends anything but a header.
//
// This test never seeds, and an unseeded randomMT reloads from a fixed default
// seed (Source/Rand.cpp), so under ctest - one process per test - the sizes repeat
// run for run. The generator is global, though, so a whole-binary `RakNetTests`
// run with no filter gets whatever the tests ahead of this one in link order left
// behind; ComprehensiveConvert and DroppedConnectionConvert both seed it to 12345
// and then draw from it.
constexpr unsigned int kMaxPadBytes = 5000;

// Lets the packets still in flight land before the counts are compared.
// RELIABLE_ORDERED promises they will, so this is a hang guard rather than a
// tuning knob; over loopback it exits in milliseconds and costs the test nothing.
constexpr TimeMS kFinalDrainBudgetMs = 5000;

// Twelve seconds at one send every 30 ms is about 400. A floor with slack rather
// than an exact count, and its job is to rule out the vacuous pass: the send loop
// is gated on ID_CONNECTION_REQUEST_ACCEPTED, so a run in which the connection
// never came up sends nothing, receives nothing, and matches zero against zero.
constexpr unsigned int kMinimumPacketsSent = 300;

} // namespace

// Wrap-safe on a uint32_t TimeMS, where a plain >= is not. See ConnectionWaits.h.
using ConnectionWaits::Expired;

TEST_CASE( "Every packet sent RELIABLE_ORDERED arrives, once, in the order it was sent", "[network]" )
{
    PeerScope peers;

    // Receiver first, so the Connect() below lands on a peer that is already
    // listening rather than paying a retry cycle out of a fixed twelve-second
    // budget - and so the connected client is destroyed before the server.
    RakPeerInterface* receiver = peers.Server( kReceiverPort, kReceiverMaxConnections );
    RakPeerInterface* sender = peers.Client();

    // Nothing else here can tell a refused request from a slow one.
    REQUIRE( sender->Connect( "127.0.0.1", kReceiverPort, 0, 0 ) == CONNECTION_ATTEMPT_STARTED );

    unsigned int packetsSent = 0;
    unsigned int packetsReceived = 0;

    bool connected = false;

    // Absolute time of the next send; only meaningful once connected.
    TimeMS nextSend = 0;

    auto handleReceived = [&]( const Packet* packet ) {
        if( packet->data[0] != kPacketId )
        {
            return;
        }

        BitStream bitStream( packet->data, packet->length, false );
        bitStream.IgnoreBytes( 1 );

        unsigned int number = 0;
        unsigned char channel = 0;

        // REQUIRE: the comparisons below are only meaningful if the fields were
        // there to read at all, and a short packet read unchecked leaves whatever
        // the stack held.
        REQUIRE( bitStream.Read( number ) );
        REQUIRE( bitStream.Read( channel ) );

        CHECK( channel == kChannel );

        // REQUIRE rather than the suite's CHECK default, because this latches: the
        // expected number advances one per packet, so a single break in the
        // ordering makes every one of the ~400 packets behind it mismatch too, and
        // one out-of-order RELIABLE_ORDERED delivery is already a complete
        // diagnosis.
        REQUIRE( number == packetsReceived );

        packetsReceived++;
    };

    auto drainReceiver = [&]() {
        for( Packet* packet = receiver->Receive(); packet; receiver->DeallocatePacket( packet ), packet = receiver->Receive() )
        {
            handleReceived( packet );
        }
    };

    const TimeMS entryTime = GetTimeMS();

    while( GetTimeMS() - entryTime < kTestDurationMs )
    {
        // The one message id this test acts on: it starts the send stream and sets
        // its clock.
        for( Packet* packet = sender->Receive(); packet; sender->DeallocatePacket( packet ), packet = sender->Receive() )
        {
            if( packet->data[0] == ID_CONNECTION_REQUEST_ACCEPTED )
            {
                connected = true;
                nextSend = GetTimeMS();
            }
        }

        while( connected && Expired( nextSend ) )
        {
            BitStream bitStream;
            bitStream.Write( kPacketId );
            bitStream.Write( packetsSent );
            bitStream.Write( kChannel );

            // Zeroed rather than uninitialised, so no heap contents go on the
            // wire. Nothing reads the padding; only its length matters.
            const std::vector<char> pad( randomMT() % kMaxPadBytes + 1, 0 );
            bitStream.Write( pad.data(), static_cast<unsigned int>( pad.size() ) );

            // Send fails on a null or empty buffer, not on a busy peer, so a false
            // here is a bug in the four lines above rather than a slow link.
            REQUIRE( sender->Send( &bitStream, HIGH_PRIORITY, RELIABLE_ORDERED, kChannel, UNASSIGNED_SYSTEM_ADDRESS, true ) );

            packetsSent++;
            nextSend += kSendIntervalMs;
        }

        drainReceiver();

        // A yield rather than a pause: the send cadence is 30 ms and the loop
        // wants to be back before the next one is due.
        std::this_thread::sleep_for( std::chrono::milliseconds( 0 ) );
    }

    // Everything below is measured against this: without it both counts are zero
    // and equal, and a run in which nothing ever connected passes.
    REQUIRE( connected );

    const TimeMS drainDeadline = GetTimeMS() + kFinalDrainBudgetMs;
    while( packetsReceived < packetsSent && !Expired( drainDeadline ) )
    {
        drainReceiver();

        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }

    INFO( "sent " << packetsSent << ", received " << packetsReceived );

    CHECK( packetsSent >= kMinimumPacketsSent );

    // Not "most of them arrived": RELIABLE_ORDERED promises all of them, and the
    // loop above has already waited out the ones that were in flight.
    CHECK( packetsReceived == packetsSent );
}
