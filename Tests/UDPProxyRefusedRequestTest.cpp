#include "Plugins/UDPProxyClient.h"
#include "Plugins/UDPProxyServer.h"

#include "BitStream.h"
#include "ConnectionWaits.h"
#include "MTUSize.h"
#include "PeerScope.h"
#include "RakNetTypes.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

/*
A UDPProxyServer or UDPProxyClient request that Send refuses never reaches the coordinator,
so the call that made it fails and keeps no record of it. A login record would make the
next login to that coordinator fail as already in progress, and a forwarding record would
take one of the MAX_OUTSTANDING_REQUESTS places until it timed out.
*/

using namespace RakNet;

namespace {

// Ports no other test uses.
constexpr unsigned short kCoordinatorPort = 31077;

constexpr TimeMS kRequestTimeoutMs = 10000;

struct SilentHandler : public UDPProxyClientResultHandler
{
    void OnForwardingSuccess( const char*, unsigned short, SystemAddress, SystemAddress, SystemAddress, RakNetGUID, UDPProxyClient* ) override {}
    void OnForwardingNotification( const char*, unsigned short, SystemAddress, SystemAddress, SystemAddress, RakNetGUID, UDPProxyClient* ) override {}
    void OnNoServersOnline( SystemAddress, SystemAddress, SystemAddress, RakNetGUID, UDPProxyClient* ) override {}
    void OnRecipientNotConnected( SystemAddress, SystemAddress, SystemAddress, RakNetGUID, UDPProxyClient* ) override {}
    void OnAllServersBusy( SystemAddress, SystemAddress, SystemAddress, RakNetGUID, UDPProxyClient* ) override {}
    void OnForwardingInProgress( const char*, unsigned short, SystemAddress, SystemAddress, SystemAddress, RakNetGUID, UDPProxyClient* ) override {}
};

} // namespace

TEST_CASE( "UDPProxyServer::LoginToCoordinator fails when its request is refused", "[udpproxy][network]" )
{
    // Before the PeerScope, so it outlives the peer.
    UDPProxyServer proxyServer;

    PeerScope peers;
    RakPeerInterface* peer = peers.Create();
    peer->AttachPlugin( &proxyServer );
    const SystemAddress coordinator( "127.0.0.1", kCoordinatorPort );

    // A stopped Peer refuses every Send.
    CHECK_FALSE( proxyServer.LoginToCoordinator( "password", coordinator ) );

    SocketDescriptor sd;
    REQUIRE( peer->Startup( 1, &sd, 1 ) == RAKNET_STARTED );
    CHECK( proxyServer.LoginToCoordinator( "password", coordinator ) );

    peer->DetachPlugin( &proxyServer );
}

TEST_CASE( "UDPProxyClient::RequestForwarding fails and keeps no request when its request is refused", "[udpproxy][network]" )
{
    // Before the PeerScope, so they outlive the peers.
    SilentHandler handler;
    UDPProxyClient proxyClient;
    proxyClient.SetResultHandler( &handler );

    PeerScope peers;
    RakPeerInterface* coordinator = peers.Server( kCoordinatorPort );
    RakPeerInterface* client = peers.Client();
    client->AttachPlugin( &proxyClient );
    ConnectionWaits::ConnectAndWait( client, coordinator );
    const SystemAddress coordinatorAddress = ConnectionWaits::LoopbackAddressOf( coordinator );

    // Send refuses a message longer than MAXIMUM_MESSAGE_SIZE.
    BitStream oversizedSelection;
    oversizedSelection.PadWithZeroToByteLength( MAXIMUM_MESSAGE_SIZE + 1 );

    // As many refusals as there are places, so a single place kept fills them all.
    for( unsigned int i = 0; i < UDPProxyClient::MAX_OUTSTANDING_REQUESTS; i++ )
    {
        // RequestForwarding copies the selection from its read pointer on.
        oversizedSelection.ResetReadPointer();
        CHECK_FALSE( proxyClient.RequestForwarding( coordinatorAddress, UNASSIGNED_SYSTEM_ADDRESS, RakNetGUID( 1000 + i ), kRequestTimeoutMs, &oversizedSelection ) );
    }

    CHECK( proxyClient.RequestForwarding( coordinatorAddress, UNASSIGNED_SYSTEM_ADDRESS, RakNetGUID( 999 ), kRequestTimeoutMs ) );

    client->DetachPlugin( &proxyClient );
}
