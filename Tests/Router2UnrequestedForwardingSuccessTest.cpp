#include "Plugins/Router2.h"

#include "BitStream.h"
#include "ConnectionWaits.h"
#include "GetTime.h"
#include "MessageIdentifiers.h"
#include "PeerScope.h"
#include "RakNetStringMakers.h"
#include "RakNetTypes.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>
#include <vector>

/*
Router2 consumes an ID_ROUTER_2_FORWARDING_ESTABLISHED naming an endpoint it never asked to
reach.

OnForwardingSuccess took a miss in forwardedConnectionList to mean a connection request was
pending, and indexed connectionRequests with whatever GetConnectionRequestIndex returned -
~0u when there was none - then erased at that index. Any connected System could trigger it
with one Message. Under MSVC debug iterators that aborts; elsewhere it is undefined
behaviour.

The injected Message is followed by a user Message on the same ordered channel, so once the
user Message comes out of Receive the injected one has been through the plugin.
*/

using namespace RakNet;

namespace {

constexpr unsigned short kServerPort = 30000;

// Hang guard for the marker Message. On loopback it arrives a few update cycles after the
// send, tens of milliseconds.
constexpr TimeMS kMarkerBudgetMs = 5000;

constexpr MessageID kMarker = ID_USER_PACKET_ENUM;

// Returns the message ids the server's Receive hands out up to and including the marker,
// or up to the deadline if the marker never comes.
std::vector<MessageID> ReceiveUntilMarker( RakPeerInterface* server )
{
    std::vector<MessageID> received;
    const TimeMS deadline = GetTimeMS() + kMarkerBudgetMs;
    while( !ConnectionWaits::Expired( deadline ) )
    {
        for( Packet* packet = server->Receive(); packet != nullptr; packet = server->Receive() )
        {
            received.push_back( packet->data[0] );
            server->DeallocatePacket( packet );
        }
        if( !received.empty() && received.back() == kMarker )
            break;
        std::this_thread::sleep_for( std::chrono::milliseconds( ConnectionWaits::kPollInterval ) );
    }
    return received;
}

bool Contains( const std::vector<MessageID>& ids, MessageID id )
{
    for( MessageID each : ids )
    {
        if( each == id )
            return true;
    }
    return false;
}

} // namespace

TEST_CASE( "Router2 consumes a forwarding success for an endpoint it never asked to reach", "[router2][network]" )
{
    // Before the PeerScope, so it outlives the peers.
    Router2 router;

    PeerScope peers;
    RakPeerInterface* server = peers.Server( kServerPort );
    RakPeerInterface* client = peers.Client();
    server->AttachPlugin( &router );

    ConnectionWaits::ConnectAndWait( client, server );

    const SystemAddress clientAddressBefore = server->GetSystemAddressFromGuid( client->GetMyGUID() );
    const SystemAddress serverAddress( "127.0.0.1", kServerPort );

    BitStream forged;
    forged.Write( (MessageID)ID_ROUTER_2_FORWARDING_ESTABLISHED );
    forged.Write( RakNetGUID( 1001 ) );
    forged.Write( (unsigned short)25000 );
    client->Send( &forged, HIGH_PRIORITY, RELIABLE_ORDERED, 0, serverAddress, false );

    BitStream marker;
    marker.Write( kMarker );
    client->Send( &marker, HIGH_PRIORITY, RELIABLE_ORDERED, 0, serverAddress, false );

    const std::vector<MessageID> received = ReceiveUntilMarker( server );

    REQUIRE( Contains( received, kMarker ) );
    CHECK( !Contains( received, ID_ROUTER_2_FORWARDING_ESTABLISHED ) );
    CHECK( !Contains( received, ID_ROUTER_2_REROUTED ) );
    CHECK( server->GetSystemAddressFromGuid( client->GetMyGUID() ) == clientAddressBefore );
    CHECK( server->GetSystemAddressFromGuid( RakNetGUID( 1001 ) ) == UNASSIGNED_SYSTEM_ADDRESS );

    server->DetachPlugin( &router );
}
