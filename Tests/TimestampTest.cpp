#include "PeerScope.h"

#include "BitStream.h"
#include "ConnectionWaits.h"
#include "MessageIdentifiers.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

/*
Pins the value Receive hands out for a Timestamped Message: the RakNet::Time after the
ID_TIMESTAMP byte, shifted by the sender's clock differential.

RakPeerInterface functions explicitly tested:

    Receive
    GetClockDifferential
*/

using namespace RakNet;

namespace {

// Ports no other test uses.
constexpr unsigned short kServerPort = 32400;
constexpr unsigned short kClientPort = 32401;

// Hang guard for each wait, not a settle time: each normally ends within a second.
constexpr TimeMS kWaitBudgetMs = 10000;

// Its bytes differ from its byte-swapped form, so a Receive that reads the bytes in the
// wrong order is caught.
constexpr RakNet::Time kSentTime = static_cast<RakNet::Time>( 0x0000'0123'4567'89ABull );

} // namespace

TEST_CASE( "Receive shifts a Timestamped Message's time by the sender's clock differential", "[network]" )
{
    PeerScope peers;
    RakPeerInterface* server = peers.Server( kServerPort );
    RakPeerInterface* client = peers.Client( kClientPort );

    const SystemAddress serverAddress( "127.0.0.1", kServerPort );
    const SystemAddress clientAddress( "127.0.0.1", kClientPort );
    ConnectionWaits::ConnectAndWait( client, server );

    BitStream message;
    message.Write( (MessageID)ID_TIMESTAMP );
    message.Write( kSentTime );
    message.Write( (MessageID)ID_USER_PACKET_ENUM );
    REQUIRE( client->Send( &message, HIGH_PRIORITY, RELIABLE_ORDERED, 0, serverAddress, false ) != 0 );

    // A ping can change the differential while the Message is in flight, so it is read on
    // both sides of the receive and the shift must match one of them.
    const RakNet::Time differentialBefore = server->GetClockDifferential( clientAddress );

    Packet* packet = ConnectionWaits::ReceiveMessage( server, ID_TIMESTAMP, kWaitBudgetMs );
    REQUIRE( packet != nullptr );
    RakNet::Time receivedTime = 0;
    BitStream in( packet->data, packet->length, false );
    in.IgnoreBytes( sizeof( MessageID ) );
    const bool read = in.Read( receivedTime );
    server->DeallocatePacket( packet );
    REQUIRE( read );

    const RakNet::Time differentialAfter = server->GetClockDifferential( clientAddress );

    INFO( "received " << receivedTime << ", sent " << kSentTime << ", differential " << differentialBefore << " then "
                      << differentialAfter );
    CHECK( ( receivedTime == kSentTime - differentialBefore || receivedTime == kSentTime - differentialAfter ) );
}
