#include "Plugins/TwoWayAuthentication.h"

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
TwoWayAuthentication::OnClosedConnection drops the closed System's pending
challenges and keeps everyone else's.

The regression: the std::deque port left an `outgoingChallenges.erase( end() )`
in front of the removal loop - undefined behaviour on every disconnect of a Peer
with the plugin attached. MSVC's debug iterators assert on it; release builds
corrupt the deque or crash.

The fixture is a server with the plugin and two plain clients. The server
challenges both, which leaves two pending challenges because neither client
answers, then one client disconnects. The plugin only hears about a closed
connection from RakPeer::Receive, as it hands the ID_DISCONNECTION_NOTIFICATION
up, so the server is polled until that packet arrives. The challenges are read
straight off outgoingChallenges, which the header leaves public.

Update's timeout sweep cannot fake a pass. It is CHALLENGE_MINIMUM_TIMEOUT
(3 s) away on a healthy run, and the two challenges are stamped back to back,
so a sweep that did fire would take both - leaving none, which fails.
*/

using namespace RakNet;

namespace {

constexpr unsigned short kServerPort = 30000;

// Polls server until it hands up an ID_DISCONNECTION_NOTIFICATION from guid,
// deallocating everything else, or the deadline passes.
bool WaitForDisconnectionNotification( RakPeerInterface* server, RakNetGUID guid )
{
    const TimeMS deadline = GetTimeMS() + ConnectionWaits::kDisconnectBudget;
    while( !ConnectionWaits::Expired( deadline ) )
    {
        for( Packet* packet = server->Receive(); packet != nullptr; packet = server->Receive() )
        {
            const bool found = packet->data[0] == ID_DISCONNECTION_NOTIFICATION && packet->guid == guid;
            server->DeallocatePacket( packet );
            if( found )
                return true;
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( ConnectionWaits::kPollInterval ) );
    }
    return false;
}

} // namespace

TEST_CASE( "TwoWayAuthentication drops only the closed System's pending challenge", "[network]" )
{
    // Before the PeerScope, so it outlives the peers: DestroyInstance calls
    // OnRakPeerShutdown on every plugin still attached, and a failing REQUIRE
    // throws past the DetachPlugin at the bottom.
    TwoWayAuthentication auth;

    PeerScope peers;
    RakPeerInterface* server = peers.Server( kServerPort, 2 );
    RakPeerInterface* closing = peers.Client();
    RakPeerInterface* staying = peers.Client();
    server->AttachPlugin( &auth );

    const SystemAddress serverAddress( "127.0.0.1", kServerPort );
    REQUIRE( closing->Connect( "127.0.0.1", kServerPort, nullptr, 0 ) == CONNECTION_ATTEMPT_STARTED );
    REQUIRE( staying->Connect( "127.0.0.1", kServerPort, nullptr, 0 ) == CONNECTION_ATTEMPT_STARTED );

    RakPeerInterface* clients[] = { closing, staying };
    ConnectionWaits::WaitForConnectionCounts( clients, 2, 1 );
    ConnectionWaits::WaitForConnectionCounts( &server, 1, 2 );

    const RakNetGUID closingGuid = closing->GetMyGUID();
    const RakNetGUID stayingGuid = staying->GetMyGUID();

    REQUIRE( auth.AddPassword( "identifier", "password" ) );
    REQUIRE( auth.Challenge( "identifier", closingGuid ) );
    REQUIRE( auth.Challenge( "identifier", stayingGuid ) );
    REQUIRE( auth.outgoingChallenges.size() == 2 );

    closing->CloseConnection( serverAddress, true, 0, LOW_PRIORITY );
    REQUIRE( WaitForDisconnectionNotification( server, closingGuid ) );

    REQUIRE( auth.outgoingChallenges.size() == 1 );
    CHECK( auth.outgoingChallenges.front().remoteSystem.rakNetGuid == stayingGuid );

    server->DetachPlugin( &auth );
}
