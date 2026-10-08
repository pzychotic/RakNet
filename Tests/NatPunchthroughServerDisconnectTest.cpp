#include "Plugins/NatPunchthroughServer.h"

#include "BitStream.h"
#include "ConnectionWaits.h"
#include "GetTime.h"
#include "MessageIdentifiers.h"
#include "PeerScope.h"
#include "RakNetStringMakers.h"
#include "RakNetTypes.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

/*
NatPunchthroughServer forgets only the User of the System that disconnected. The others stay
reachable as punchthrough targets.

The disconnected System is the middle one of three in RakNetGUID order, so a removal that
shifted or skipped its neighbours in a sorted list would lose the one after it.

A request between the two remaining Systems is accepted, which the server answers with
ID_NAT_GET_MOST_RECENT_PORT. A request to the one that left is refused with
ID_NAT_TARGET_NOT_CONNECTED.
*/

using namespace RakNet;

namespace {

constexpr unsigned short kServerPort = 30000;

// Hang guard for the server's answer. On loopback it arrives a few update cycles after the
// request, tens of milliseconds.
constexpr TimeMS kAnswerBudgetMs = 5000;

void SendPunchthroughRequest( RakPeerInterface* client, RakNetGUID target, const SystemAddress& server )
{
    BitStream request;
    request.Write( (MessageID)ID_NAT_PUNCHTHROUGH_REQUEST );
    request.Write( target );
    client->Send( &request, HIGH_PRIORITY, RELIABLE_ORDERED, 0, server, false );
}

// The first NAT message the client receives, or ID_USER_PACKET_ENUM if none arrives in time.
MessageID FirstNatAnswer( RakPeerInterface* server, RakPeerInterface* client )
{
    const TimeMS deadline = GetTimeMS() + kAnswerBudgetMs;
    while( !ConnectionWaits::Expired( deadline ) )
    {
        ConnectionWaits::Drain( server );
        for( Packet* packet = client->Receive(); packet != nullptr; packet = client->Receive() )
        {
            const MessageID id = packet->data[0];
            client->DeallocatePacket( packet );
            if( id >= ID_NAT_PUNCHTHROUGH_REQUEST && id <= ID_NAT_PUNCHTHROUGH_SUCCEEDED )
                return id;
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( ConnectionWaits::kPollInterval ) );
    }
    return ID_USER_PACKET_ENUM;
}

} // namespace

TEST_CASE( "NatPunchthroughServer keeps the other users when the middle one disconnects", "[natpunchthrough][network]" )
{
    // Before the PeerScope, so it outlives the peers.
    NatPunchthroughServer punchServer;

    PeerScope peers;
    RakPeerInterface* server = peers.Server( kServerPort, 3 );
    server->AttachPlugin( &punchServer );

    std::vector<RakPeerInterface*> clients = { peers.Client(), peers.Client(), peers.Client() };
    for( RakPeerInterface* client : clients )
        ConnectionWaits::ConnectAndWait( client, server );
    std::sort( clients.begin(), clients.end(), []( RakPeerInterface* a, RakPeerInterface* b ) { return a->GetMyGUID() < b->GetMyGUID(); } );
    RakPeerInterface* first = clients[0];
    RakPeerInterface* middle = clients[1];
    RakPeerInterface* last = clients[2];
    const RakNetGUID middleGuid = middle->GetMyGUID();

    // The plugin sees the disconnect from Receive, so the server is drained until the
    // notification comes out of it, which is after every plugin's OnClosedConnection has run.
    const SystemAddress serverAddress( "127.0.0.1", kServerPort );
    middle->CloseConnection( serverAddress, true );
    REQUIRE( ConnectionWaits::WaitForMessage( server, ID_DISCONNECTION_NOTIFICATION, kAnswerBudgetMs ) );

    SendPunchthroughRequest( first, middleGuid, serverAddress );
    CHECK( FirstNatAnswer( server, first ) == ID_NAT_TARGET_NOT_CONNECTED );

    SendPunchthroughRequest( first, last->GetMyGUID(), serverAddress );
    CHECK( FirstNatAnswer( server, first ) == ID_NAT_GET_MOST_RECENT_PORT );

    server->DetachPlugin( &punchServer );
}
