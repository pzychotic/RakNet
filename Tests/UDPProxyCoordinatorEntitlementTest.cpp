#include "Plugins/UDPProxyCoordinator.h"
#include "Plugins/UDPProxyCommon.h"

#include "BitStream.h"
#include "ConnectionWaits.h"
#include "GetTime.h"
#include "MarkerInjection.h"
#include "MessageIdentifiers.h"
#include "PeerScope.h"
#include "RakNetStringMakers.h"
#include "RakNetTypes.h"
#include "RakPeerInterface.h"
#include "UDPProxyWire.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <thread>

/*
UDPProxyCoordinator does not honour a claim about a third party (ADR-0006).

A forwarding request carries a source address, and stock used it as-is, so any connected
System could have a proxy server forward from an address that is not its own. The source is
now always the requester as the coordinator sees it. A ping reply names the (source, target)
pair it answers for, and stock filed every sender other than the source as the target, so any
connected System could choose which proxy server another pair was given. A reply now counts
only from one of the pair's own ends.

The clients and proxy servers are plain Peers that write the Messages UDPProxyClient and
UDPProxyServer would. Each injected Message is checked through MarkerInjection.
*/

using namespace RakNet;
using namespace UDPProxyWire;

namespace {

constexpr unsigned short kCoordinatorPort = 30000;

// Hang guard for a reply. On loopback it arrives a few update cycles after the send, tens of
// milliseconds.
constexpr TimeMS kStepBudgetMs = 5000;

const char* const kPassword = "password";

const SystemAddress kCoordinatorAddress( "127.0.0.1", kCoordinatorPort );
// Neither is connected to the coordinator.
const SystemAddress kForeignSource( "10.0.2.1", 3001 );
const SystemAddress kUnconnectedTarget( "10.0.1.2", 2002 );

// Reaches the forwarding request list and the logged-in servers.
class CoordinatorProbe : public UDPProxyCoordinator
{
public:
    ForwardingRequest* Find( const SystemAddress& source, const SystemAddress& target )
    {
        SenderAndTargetAddress sata;
        sata.senderClientAddress = source;
        sata.targetClientAddress = target;
        bool objectExists;
        const unsigned int index = forwardingRequestList.GetIndexFromKey( sata, &objectExists );
        return objectExists ? forwardingRequestList[index] : nullptr;
    }
    unsigned int RequestCount() const { return forwardingRequestList.Size(); }
};

// Receives on the coordinator and on peer until peer's Receive hands out the ID_UDP_PROXY_GENERAL
// Message with subId, and copies it into out. Anything else peer receives is dropped.
bool AwaitProxyMessage( RakPeerInterface* coordinator, RakPeerInterface* peer, MessageID subId, BitStream& out )
{
    const TimeMS deadline = GetTimeMS() + kStepBudgetMs;
    while( !ConnectionWaits::Expired( deadline ) )
    {
        ConnectionWaits::Drain( coordinator );
        for( Packet* packet = peer->Receive(); packet != nullptr; packet = peer->Receive() )
        {
            const bool found = packet->length > 1 && packet->data[0] == ID_UDP_PROXY_GENERAL && packet->data[1] == subId;
            if( found )
                out.Write( (const char*)packet->data, packet->length );
            peer->DeallocatePacket( packet );
            if( found )
                return true;
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( ConnectionWaits::kPollInterval ) );
    }
    return false;
}

// Logs server in as a proxy server, as UDPProxyServer::LoginToCoordinator does.
void Login( RakPeerInterface* coordinator, RakPeerInterface* server )
{
    BitStream login;
    login.Write( (MessageID)ID_UDP_PROXY_GENERAL );
    login.Write( (MessageID)ID_UDP_PROXY_LOGIN_REQUEST_FROM_SERVER_TO_COORDINATOR );
    login.Write( std::string( kPassword ) );
    server->Send( &login, HIGH_PRIORITY, RELIABLE_ORDERED, 0, kCoordinatorAddress, false );

    BitStream reply;
    REQUIRE( AwaitProxyMessage( coordinator, server, ID_UDP_PROXY_LOGIN_SUCCESS_FROM_COORDINATOR_TO_SERVER, reply ) );
}

// The request a proxy server is sent, as the (source, target) it names.
struct ServerRequest
{
    SystemAddress source;
    SystemAddress target;
};

ServerRequest AwaitServerRequest( RakPeerInterface* coordinator, RakPeerInterface* server )
{
    BitStream message;
    REQUIRE( AwaitProxyMessage( coordinator, server, ID_UDP_PROXY_FORWARDING_REQUEST_FROM_COORDINATOR_TO_SERVER, message ) );
    message.IgnoreBytes( 2 );
    ServerRequest request;
    REQUIRE( message.Read( request.source ) );
    REQUIRE( message.Read( request.target ) );
    return request;
}

} // namespace

TEST_CASE( "UDPProxyCoordinator does not honour a claim about a third party", "[udpproxy][network]" )
{
    // Before the PeerScope, so it outlives the peers.
    CoordinatorProbe coordinatorPlugin;
    coordinatorPlugin.SetRemoteLoginPassword( kPassword );

    PeerScope peers;
    RakPeerInterface* coordinator = peers.Server( kCoordinatorPort, 8 );
    RakPeerInterface* serverA = peers.Client();
    RakPeerInterface* serverB = peers.Client();
    RakPeerInterface* source = peers.Client();
    RakPeerInterface* target = peers.Client();
    RakPeerInterface* other = peers.Client();
    coordinator->AttachPlugin( &coordinatorPlugin );

    for( RakPeerInterface* peer : { serverA, serverB, source, target, other } )
        ConnectionWaits::ConnectAndWait( peer, coordinator );
    // Each as the coordinator sees it, which is what it writes and keys on.
    const SystemAddress serverAAddress = coordinator->GetSystemAddressFromGuid( serverA->GetMyGUID() );
    const SystemAddress serverBAddress = coordinator->GetSystemAddressFromGuid( serverB->GetMyGUID() );
    const SystemAddress sourceAddress = coordinator->GetSystemAddressFromGuid( source->GetMyGUID() );
    const SystemAddress targetAddress = coordinator->GetSystemAddressFromGuid( target->GetMyGUID() );

    SECTION( "A forwarding request naming a foreign source forwards from the requester" )
    {
        // One proxy server, so the coordinator asks it straight away.
        Login( coordinator, serverA );

        // The target is not connected, which a request by address allows.
        BitStream request;
        WriteForwardingRequest( request, kForeignSource, kUnconnectedTarget );
        MarkerInjection::Inject( source, coordinator, request );

        const ServerRequest forwarded = AwaitServerRequest( coordinator, serverA );
        CHECK( forwarded.source == sourceAddress );
        CHECK( forwarded.target == kUnconnectedTarget );
        CHECK( coordinatorPlugin.Find( kForeignSource, kUnconnectedTarget ) == nullptr );
    }

    SECTION( "A ping reply counts only from the pair's own ends" )
    {
        // Two proxy servers, so the coordinator asks both ends for pings first.
        Login( coordinator, serverA );
        Login( coordinator, serverB );

        BitStream request;
        WriteForwardingRequest( request, UNASSIGNED_SYSTEM_ADDRESS, target->GetMyGUID() );
        MarkerInjection::Inject( source, coordinator, request );
        UDPProxyCoordinator::ForwardingRequest* fw = coordinatorPlugin.Find( sourceAddress, targetAddress );
        REQUIRE( fw != nullptr );
        REQUIRE( fw->timeRequestedPings != 0 );

        // B looks far better, to a System that is neither end.
        BitStream forged;
        WritePingReply( forged, sourceAddress, targetAddress, { ServerPing( serverAAddress, 900 ), ServerPing( serverBAddress, 1 ) } );
        MarkerInjection::Inject( other, coordinator, forged );
        CHECK( fw->sourceServerPings.empty() );
        CHECK( fw->targetServerPings.empty() );

        BitStream fromSource;
        WritePingReply( fromSource, sourceAddress, targetAddress, { ServerPing( serverAAddress, 10 ), ServerPing( serverBAddress, 50 ) } );
        MarkerInjection::Inject( source, coordinator, fromSource );
        CHECK( fw->sourceServerPings.size() == 2 );
        CHECK( fw->targetServerPings.empty() );

        // With both ends in, the coordinator tries the server with the lower summed ping.
        BitStream fromTarget;
        WritePingReply( fromTarget, sourceAddress, targetAddress, { ServerPing( serverAAddress, 20 ), ServerPing( serverBAddress, 60 ) } );
        MarkerInjection::Inject( target, coordinator, fromTarget );
        CHECK( fw->targetServerPings.size() == 2 );

        const ServerRequest forwarded = AwaitServerRequest( coordinator, serverA );
        CHECK( forwarded.source == sourceAddress );
        CHECK( forwarded.target == targetAddress );
    }

    coordinator->DetachPlugin( &coordinatorPlugin );
}
