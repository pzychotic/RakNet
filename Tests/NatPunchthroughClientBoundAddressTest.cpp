#include "Plugins/NatPunchthroughClient.h"

#include "BitStream.h"
#include "ConnectionWaits.h"
#include "GetTime.h"
#include "MessageIdentifiers.h"
#include "PeerScope.h"
#include "RakNetTypes.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

/*
NatPunchthroughClient skips a bound address that is UNASSIGNED_SYSTEM_ADDRESS when it pings
the facilitator's bound addresses, and pings the next one instead.

It used to write that address out as the text "UNASSIGNED_SYSTEM_ADDRESS" and hand it to
SendOutOfBand, which looked it up as a host name, failed, and sent nothing. The loop stops at
the first address it pings, so the real address after it was never pinged.

The facilitator is a plain peer standing in for NatPunchthroughServer. It answers the
client's ID_NAT_REQUEST_BOUND_ADDRESSES with two addresses: UNASSIGNED_SYSTEM_ADDRESS, then
the loopback address of a third peer, which reads the ID_NAT_PING the client sends it.
*/

using namespace RakNet;

namespace {

// Ports no other test uses.
constexpr unsigned short kFacilitatorPort = 31073;

// Hang guard. On loopback the ping arrives a few update cycles after the answer.
constexpr TimeMS kPingBudgetMs = 5000;

bool IsNatPing( const Packet* packet )
{
    return packet->length >= 2 && packet->data[0] == ID_OUT_OF_BAND_INTERNAL && packet->data[1] == ID_NAT_PING;
}

} // namespace

TEST_CASE( "NatPunchthroughClient skips an unassigned bound address and pings the next", "[network]" )
{
    // Before the PeerScope, so it outlives the peers.
    NatPunchthroughClient punch;

    PeerScope peers;
    RakPeerInterface* facilitator = peers.Server( kFacilitatorPort );
    RakPeerInterface* client = peers.Client();
    RakPeerInterface* pinged = peers.Client();
    client->AttachPlugin( &punch );

    ConnectionWaits::ConnectAndWait( client, facilitator );

    // An unknown port stride makes OpenNAT ask for the bound addresses first.
    REQUIRE( punch.OpenNAT( RakNetGUID( 1001 ), SystemAddress( "127.0.0.1", kFacilitatorPort ) ) );

    const SystemAddress pingedAddress = ConnectionWaits::LoopbackAddressOf( pinged );

    bool sawPing = false;
    const TimeMS deadline = GetTimeMS() + kPingBudgetMs;
    while( !sawPing && !ConnectionWaits::Expired( deadline ) )
    {
        for( Packet* packet = facilitator->Receive(); packet != nullptr; packet = facilitator->Receive() )
        {
            if( packet->data[0] == ID_NAT_REQUEST_BOUND_ADDRESSES )
            {
                BitStream answer;
                answer.Write( (MessageID)ID_NAT_RESPOND_BOUND_ADDRESSES );
                answer.Write( (unsigned char)2 );
                answer.Write( UNASSIGNED_SYSTEM_ADDRESS );
                answer.Write( pingedAddress );
                CHECK( facilitator->Send( &answer, HIGH_PRIORITY, RELIABLE_ORDERED, 0, packet->systemAddress, false ) != 0 );
            }
            facilitator->DeallocatePacket( packet );
        }
        ConnectionWaits::Drain( client );
        for( Packet* packet = pinged->Receive(); packet != nullptr; packet = pinged->Receive() )
        {
            if( IsNatPing( packet ) )
                sawPing = true;
            pinged->DeallocatePacket( packet );
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( ConnectionWaits::kPollInterval ) );
    }

    CHECK( sawPing );

    client->DetachPlugin( &punch );
}
