#include "PeerScope.h"

#include "ConnectionWaits.h"
#include "MessageIdentifiers.h"
#include "RakNetStringMakers.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

/*
Pins the Peer's external address, unqualified, to the published view (ADR-0007).

- In the ID_CONNECTION_REQUEST_ACCEPTED handler the external address is already set, and a
  Send to it comes back through Receive as loopback.
- Once the Peer shuts down it has no external address.

RakPeerInterface functions explicitly tested:

    GetExternalID
    Send
*/

using namespace RakNet;

namespace {

constexpr unsigned short kLoopbackServerPort = 32800;
constexpr unsigned short kLoopbackClientPort = 32801;
constexpr unsigned short kShutdownServerPort = 32810;
constexpr unsigned short kShutdownClientPort = 32811;

// Hang guard for each wait, not a settle time: each normally ends within a second.
constexpr TimeMS kWaitBudgetMs = 10000;

constexpr unsigned char kLoopbackMessageId = ID_USER_PACKET_ENUM;

} // namespace

TEST_CASE( "The external address is set in the connection-accepted handler, and a Send to it is loopback", "[network]" )
{
    PeerScope peers;
    peers.Server( kLoopbackServerPort );
    RakPeerInterface* client = peers.Client( kLoopbackClientPort );

    REQUIRE( client->Connect( "127.0.0.1", kLoopbackServerPort, 0, 0 ) == CONNECTION_ATTEMPT_STARTED );

    Packet* accepted = ConnectionWaits::ReceiveMessage( client, ID_CONNECTION_REQUEST_ACCEPTED, kWaitBudgetMs );
    REQUIRE( accepted != nullptr );
    const SystemAddress externalAddress = client->GetExternalID( UNASSIGNED_SYSTEM_ADDRESS );
    client->DeallocatePacket( accepted );
    REQUIRE( externalAddress != UNASSIGNED_SYSTEM_ADDRESS );
    CHECK( externalAddress.GetPort() == kLoopbackClientPort );

    const char message[] = { (char)kLoopbackMessageId, 'x' };
    REQUIRE( client->Send( message, (int)sizeof( message ), HIGH_PRIORITY, RELIABLE_ORDERED, 0, externalAddress, false ) != 0 );

    Packet* looped = ConnectionWaits::ReceiveMessage( client, kLoopbackMessageId, kWaitBudgetMs );
    REQUIRE( looped != nullptr );
    CHECK( looped->length == sizeof( message ) );
    CHECK( looped->guid == client->GetMyGUID() );
    client->DeallocatePacket( looped );
}

TEST_CASE( "A Peer that has shut down has no external address", "[network]" )
{
    PeerScope peers;
    peers.Server( kShutdownServerPort );
    RakPeerInterface* client = peers.Client( kShutdownClientPort );

    REQUIRE( client->Connect( "127.0.0.1", kShutdownServerPort, 0, 0 ) == CONNECTION_ATTEMPT_STARTED );

    Packet* accepted = ConnectionWaits::ReceiveMessage( client, ID_CONNECTION_REQUEST_ACCEPTED, kWaitBudgetMs );
    REQUIRE( accepted != nullptr );
    client->DeallocatePacket( accepted );
    REQUIRE( client->GetExternalID( UNASSIGNED_SYSTEM_ADDRESS ) != UNASSIGNED_SYSTEM_ADDRESS );

    client->Shutdown( 0 );

    CHECK( client->GetExternalID( UNASSIGNED_SYSTEM_ADDRESS ) == UNASSIGNED_SYSTEM_ADDRESS );
}
