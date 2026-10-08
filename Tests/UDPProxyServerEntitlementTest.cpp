#include "Plugins/UDPProxyServer.h"
#include "Plugins/UDPProxyCommon.h"

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

/*
A UDPProxyServer opens forwarding only for a coordinator it has logged in to.

The check looked the sender up in loggedInCoordinators and compared the index it got back
with (unsigned)-1. A miss returns an insertion index, never that, so every System could have
the server open forwarding between any two addresses and get the port back.
*/

using namespace RakNet;

namespace {

constexpr unsigned short kProxyServerPort = 30000;

constexpr TimeMS kForwardingTimeoutMs = 10000;

// How long to wait for a reply that should never come. The request is reliable, so the
// server has it and has answered, if it will, well inside this.
constexpr TimeMS kSilenceMs = 500;

bool IsForwardingReply( const Packet& packet )
{
    return packet.length > 1 && packet.data[0] == ID_UDP_PROXY_GENERAL && packet.data[1] == ID_UDP_PROXY_FORWARDING_REPLY_FROM_SERVER_TO_COORDINATOR;
}

} // namespace

TEST_CASE( "A UDPProxyServer ignores a forwarding request from a system it has not logged in to", "[udpproxy][network]" )
{
    // Before the PeerScope, so it outlives the peers.
    UDPProxyServer proxyServer;

    PeerScope peers;
    RakPeerInterface* server = peers.Server( kProxyServerPort );
    RakPeerInterface* stranger = peers.Client();
    server->AttachPlugin( &proxyServer );

    ConnectionWaits::ConnectAndWait( stranger, server );

    BitStream request;
    request.Write( (MessageID)ID_UDP_PROXY_GENERAL );
    request.Write( (MessageID)ID_UDP_PROXY_FORWARDING_REQUEST_FROM_COORDINATOR_TO_SERVER );
    request.Write( SystemAddress( "10.0.1.1", 2001 ) );
    request.Write( SystemAddress( "10.0.1.2", 2002 ) );
    request.Write( kForwardingTimeoutMs );
    stranger->Send( &request, MEDIUM_PRIORITY, RELIABLE_ORDERED, 0, SystemAddress( "127.0.0.1", kProxyServerPort ), false );

    bool replied = false;
    const TimeMS deadline = GetTimeMS() + kSilenceMs;
    while( !ConnectionWaits::Expired( deadline ) && !replied )
    {
        ConnectionWaits::Drain( server );
        for( Packet* packet = stranger->Receive(); packet != nullptr; packet = stranger->Receive() )
        {
            replied = replied || IsForwardingReply( *packet );
            stranger->DeallocatePacket( packet );
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( ConnectionWaits::kPollInterval ) );
    }

    CHECK_FALSE( replied );

    server->DetachPlugin( &proxyServer );
}
