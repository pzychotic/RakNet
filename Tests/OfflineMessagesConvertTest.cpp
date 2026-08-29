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
#include "GetTime.h"
#include "MessageIdentifiers.h"
#include "RakNetTime.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

/*
Sending messages to systems you are not connected to. Neither peer ever connects:
peer1 advertises itself to peer2 with a payload, peer2 answers by pinging peer1
offline, and peer1's canned offline ping response comes back with the pong.

RakPeerInterface functions explicitly tested:

    SetOfflinePingResponse
    GetOfflinePingResponse
    AdvertiseSystem
    Ping

Exercised indirectly by getting to that point: Startup,
SetMaximumIncomingConnections, Receive, DeallocatePacket.
*/

using namespace RakNet;

namespace {

constexpr unsigned short kAdvertiserPort = 60001;
constexpr unsigned short kListenerPort = 60002;

constexpr char kOfflinePingResponse[] = "Offline Ping Data";
constexpr char kAdvertisedData[] = "hello world";

// Budget for the exchange below, not a settle time: both messages normally
// arrive in well under a second.
constexpr TimeMS kTestDurationMs = 10000;

// Both payloads are sent NUL-terminated, so they read back as C strings - but
// bounded by the packet rather than trusting the terminator, because a short
// or malformed packet need not carry one.
std::string PayloadAsString( const Packet* packet, size_t headerBytes )
{
    if( packet->length <= headerBytes )
    {
        return std::string();
    }

    const char* payload = reinterpret_cast<const char*>( packet->data ) + headerBytes;

    return std::string( payload, strnlen( payload, packet->length - headerBytes ) );
}

} // namespace

TEST_CASE( "AdvertiseSystem's payload and SetOfflinePingResponse's data both reach an unconnected peer", "[network]" )
{
    PeerScope peers;

    RakPeerInterface* advertiser = peers.Server( kAdvertiserPort );
    RakPeerInterface* listener = peers.Client( kListenerPort );

    advertiser->SetOfflinePingResponse( kOfflinePingResponse, static_cast<unsigned int>( strlen( kOfflinePingResponse ) ) + 1 );

    char* storedResponse = nullptr;
    unsigned int storedLength = 0;
    advertiser->GetOfflinePingResponse( &storedResponse, &storedLength );

    // REQUIRE: the comparison reads through this pointer.
    REQUIRE( storedResponse != nullptr );
    CHECK( std::string( storedResponse, strnlen( storedResponse, storedLength ) ) == kOfflinePingResponse );

    // Startup has already returned RAKNET_STARTED for both peers, but an
    // advertisement sent into a socket thread that has not come up yet is simply
    // lost, and there is no retry below.
    std::this_thread::sleep_for( std::chrono::milliseconds( 300 ) );

    advertiser->AdvertiseSystem( "127.0.0.1", kListenerPort, kAdvertisedData, static_cast<int>( strlen( kAdvertisedData ) ) + 1 );

    bool gotAdvertisement = false;
    std::string advertisedData;

    bool gotPong = false;
    std::string pingResponse;

    const TimeMS entryTime = GetTimeMS();
    while( !( gotAdvertisement && gotPong ) && GetTimeMS() - entryTime < kTestDurationMs )
    {
        // The advertiser has a queue of its own - the listener's Ping arrives
        // there as ID_UNCONNECTED_PING - and nothing asserts on it, so it is
        // drained rather than left to grow for ten seconds.
        ConnectionWaits::Drain( advertiser );

        if( Packet* packet = listener->Receive() )
        {
            if( packet->data[0] == ID_ADVERTISE_SYSTEM )
            {
                gotAdvertisement = true;
                advertisedData = PayloadAsString( packet, sizeof( unsigned char ) );

                // The advertisement is what tells the listener there is something
                // at kAdvertiserPort worth pinging.
                listener->Ping( "127.0.0.1", kAdvertiserPort, false );
            }
            else if( packet->data[0] == ID_UNCONNECTED_PONG )
            {
                gotPong = true;
                pingResponse = PayloadAsString( packet, sizeof( unsigned char ) + sizeof( TimeMS ) );
            }

            listener->DeallocatePacket( packet );
        }

        std::this_thread::sleep_for( std::chrono::milliseconds( 30 ) );
    }

    // Two questions per message, kept apart: did it arrive, and was it the right
    // bytes.
    CHECK( gotAdvertisement );
    CHECK( advertisedData == kAdvertisedData );

    CHECK( gotPong );
    CHECK( pingResponse == kOfflinePingResponse );
}
