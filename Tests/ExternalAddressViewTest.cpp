#include "PeerScope.h"

#include "ConnectionWaits.h"
#include "GetTime.h"
#include "MessageIdentifiers.h"
#include "RakNetStringMakers.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

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

/// Receive on \a client and drain \a server until \a client takes a Message with
/// \a messageId. Returns false at the deadline.
bool ReceiveUntil( RakPeerInterface* client, RakPeerInterface* server, unsigned char messageId, Packet** received )
{
    const TimeMS deadline = GetTimeMS() + kWaitBudgetMs;
    while( ConnectionWaits::Expired( deadline ) == false )
    {
        for( Packet* packet = client->Receive(); packet != 0; packet = client->Receive() )
        {
            if( packet->data[0] == messageId )
            {
                *received = packet;
                return true;
            }
            client->DeallocatePacket( packet );
        }
        ConnectionWaits::Drain( server );
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }
    return false;
}

} // namespace

TEST_CASE( "The external address is set in the connection-accepted handler, and a Send to it is loopback", "[network]" )
{
    PeerScope peers;
    RakPeerInterface* server = peers.Server( kLoopbackServerPort );
    RakPeerInterface* client = peers.Client( kLoopbackClientPort );

    REQUIRE( client->Connect( "127.0.0.1", kLoopbackServerPort, 0, 0 ) == CONNECTION_ATTEMPT_STARTED );

    Packet* accepted = 0;
    REQUIRE( ReceiveUntil( client, server, ID_CONNECTION_REQUEST_ACCEPTED, &accepted ) );
    const SystemAddress externalAddress = client->GetExternalID( UNASSIGNED_SYSTEM_ADDRESS );
    client->DeallocatePacket( accepted );
    REQUIRE( externalAddress != UNASSIGNED_SYSTEM_ADDRESS );
    CHECK( externalAddress.GetPort() == kLoopbackClientPort );

    const char message[] = { (char)kLoopbackMessageId, 'x' };
    REQUIRE( client->Send( message, (int)sizeof( message ), HIGH_PRIORITY, RELIABLE_ORDERED, 0, externalAddress, false ) != 0 );

    Packet* looped = 0;
    REQUIRE( ReceiveUntil( client, server, kLoopbackMessageId, &looped ) );
    CHECK( looped->length == sizeof( message ) );
    CHECK( looped->guid == client->GetMyGUID() );
    client->DeallocatePacket( looped );
}

TEST_CASE( "A Peer that has shut down has no external address", "[network]" )
{
    PeerScope peers;
    RakPeerInterface* server = peers.Server( kShutdownServerPort );
    RakPeerInterface* client = peers.Client( kShutdownClientPort );

    REQUIRE( client->Connect( "127.0.0.1", kShutdownServerPort, 0, 0 ) == CONNECTION_ATTEMPT_STARTED );

    Packet* accepted = 0;
    REQUIRE( ReceiveUntil( client, server, ID_CONNECTION_REQUEST_ACCEPTED, &accepted ) );
    client->DeallocatePacket( accepted );
    REQUIRE( client->GetExternalID( UNASSIGNED_SYSTEM_ADDRESS ) != UNASSIGNED_SYSTEM_ADDRESS );

    client->Shutdown( 0 );

    CHECK( client->GetExternalID( UNASSIGNED_SYSTEM_ADDRESS ) == UNASSIGNED_SYSTEM_ADDRESS );
}
