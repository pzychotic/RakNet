#include "Plugins/NatTypeDetectionClient.h"
#include "Plugins/NatTypeDetectionServer.h"

#include "ConnectionWaits.h"
#include "GetTime.h"
#include "MessageIdentifiers.h"
#include "PeerScope.h"
#include "RakNetSocket2.h"
#include "RakNetTypes.h"
#include "RakPeerInterface.h"
#include "SocketDefines.h"
#include "SocketIncludes.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

/*
NatTypeDetectionServer binds s1p2, and NatTypeDetectionClient binds c2, on the same host as
the peer's first socket. They used to take that host from GetBoundAddress, which reports
127.0.0.1 for a wildcard-bound socket, so a peer started the default way got plugin sockets
that could only talk to the local machine. The plugin socket must be bound to what the
peer's socket is really bound to: INADDR_ANY for a wildcard peer, and the same address for a
peer bound to one.

Any of those sockets can also fail to bind: a nonRakNetIP that is not on the host, or an
IPv6-bound peer, since the plugins are IPv4 only. The client used to dereference a null c2 in
DetectNATType, and the server a null s2p3, s3p4 or s4p5 in Update. Both must answer
NAT_TYPE_UNKNOWN instead.
*/

using namespace RakNet;

namespace
{

// TEST-NET-1 (RFC 5737), which is never assigned to a host, so binding to it fails.
const char* const kUnboundIP = "192.0.2.1";

const unsigned short kServerPort = 60000;
const TimeMS kResultBudgetMs = 5000;

class ExposedServer : public NatTypeDetectionServer
{
public:
    RakNetSocket2* S1p2() const { return s1p2; }
    size_t AttemptCount() const { return natDetectionAttempts.size(); }
};

class ExposedClient : public NatTypeDetectionClient
{
public:
    RakNetSocket2* C2() const { return c2; }
    bool InProgress() const { return IsInProgress(); }
};

// A client whose c2 fails to bind, which an IPv4 peer cannot make happen on its own.
class UnboundC2Client : public ExposedClient
{
protected:
    RakNetSocket2* CreateC2Socket( void ) override { return 0; }
};

// The IPv4 address the socket is bound to, in network order, as the OS reports it.
unsigned long BoundIPv4( RakNetSocket2* socket )
{
    REQUIRE( socket != 0 );
    REQUIRE( socket->IsBerkleySocket() );
    sockaddr_in sa;
    memset( &sa, 0, sizeof( sa ) );
    socklen_t len = sizeof( sa );
    REQUIRE( getsockname__( ( (RNS2_Berkley*)socket )->GetSocket(), (sockaddr*)&sa, &len ) == 0 );
    REQUIRE( sa.sin_family == AF_INET );
    return sa.sin_addr.s_addr;
}

RakPeerInterface* LoopbackPeer( PeerScope& peers )
{
    RakPeerInterface* peer = peers.Create();
    SocketDescriptor sd( 0, "127.0.0.1" );
    REQUIRE( peer->Startup( 1, &sd, 1 ) == RAKNET_STARTED );
    return peer;
}

// Polls client, and server if there is one, until client receives ID_NAT_TYPE_DETECTION_RESULT
// or the budget runs out. Returns the result, or -1 if none arrived.
int WaitForDetectionResult( RakPeerInterface* client, RakPeerInterface* server )
{
    const TimeMS deadline = GetTimeMS() + kResultBudgetMs;
    while( !ConnectionWaits::Expired( deadline ) )
    {
        if( server != 0 )
            ConnectionWaits::Drain( server );
        for( Packet* packet = client->Receive(); packet != nullptr; packet = client->Receive() )
        {
            if( packet->data[0] == ID_NAT_TYPE_DETECTION_RESULT && packet->length >= 2 )
            {
                const int result = packet->data[1];
                client->DeallocatePacket( packet );
                return result;
            }
            client->DeallocatePacket( packet );
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( ConnectionWaits::kPollInterval ) );
    }
    return -1;
}

} // namespace

TEST_CASE( "NatTypeDetectionServer binds s1p2 to INADDR_ANY when the peer is wildcard-bound", "[nattypedetection][network]" )
{
    PeerScope peers;
    RakPeerInterface* peer = peers.Client();

    ExposedServer server;
    peer->AttachPlugin( &server );
    server.Startup( "127.0.0.1", "127.0.0.1", "127.0.0.1" );

    CHECK( BoundIPv4( server.S1p2() ) == htonl( INADDR_ANY ) );

    peer->DetachPlugin( &server );
}

TEST_CASE( "NatTypeDetectionServer binds s1p2 to the peer's address when the peer is bound to one", "[nattypedetection][network]" )
{
    PeerScope peers;
    RakPeerInterface* peer = LoopbackPeer( peers );

    ExposedServer server;
    peer->AttachPlugin( &server );
    server.Startup( "127.0.0.1", "127.0.0.1", "127.0.0.1" );

    CHECK( BoundIPv4( server.S1p2() ) == htonl( INADDR_LOOPBACK ) );

    peer->DetachPlugin( &server );
}

TEST_CASE( "NatTypeDetectionClient binds c2 to INADDR_ANY when the peer is wildcard-bound", "[nattypedetection][network]" )
{
    PeerScope peers;
    RakPeerInterface* peer = peers.Client();

    ExposedClient client;
    peer->AttachPlugin( &client );
    client.DetectNATType( SystemAddress( "127.0.0.1", 1 ) );

    CHECK( BoundIPv4( client.C2() ) == htonl( INADDR_ANY ) );

    peer->DetachPlugin( &client );
}

TEST_CASE( "NatTypeDetectionClient binds c2 to the peer's address when the peer is bound to one", "[nattypedetection][network]" )
{
    PeerScope peers;
    RakPeerInterface* peer = LoopbackPeer( peers );

    ExposedClient client;
    peer->AttachPlugin( &client );
    client.DetectNATType( SystemAddress( "127.0.0.1", 1 ) );

    CHECK( BoundIPv4( client.C2() ) == htonl( INADDR_LOOPBACK ) );

    peer->DetachPlugin( &client );
}

TEST_CASE( "NatTypeDetectionServer answers NAT_TYPE_UNKNOWN when one of its sockets failed to bind", "[nattypedetection][network]" )
{
    const int unbound = GENERATE( 2, 3, 4 );
    INFO( "nonRakNetIP" << unbound << " = " << kUnboundIP );

    // Before the PeerScope, so they outlive the peers: a failing REQUIRE throws past the
    // DetachPlugin calls at the bottom.
    ExposedServer server;
    ExposedClient client;

    PeerScope peers;
    RakPeerInterface* serverPeer = peers.Server( kServerPort );
    RakPeerInterface* clientPeer = peers.Client();
    serverPeer->AttachPlugin( &server );
    clientPeer->AttachPlugin( &client );

    server.Startup( unbound == 2 ? kUnboundIP : "127.0.0.1", unbound == 3 ? kUnboundIP : "127.0.0.1", unbound == 4 ? kUnboundIP : "127.0.0.1" );

    REQUIRE( clientPeer->Connect( "127.0.0.1", kServerPort, nullptr, 0 ) == CONNECTION_ATTEMPT_STARTED );
    ConnectionWaits::WaitForConnectionCounts( &clientPeer, 1, 1 );
    ConnectionWaits::WaitForConnectionCounts( &serverPeer, 1, 1 );

    client.DetectNATType( SystemAddress( "127.0.0.1", kServerPort ) );

    CHECK( WaitForDetectionResult( clientPeer, serverPeer ) == NAT_TYPE_UNKNOWN );
    CHECK_FALSE( client.InProgress() );
    CHECK( server.AttemptCount() == 0 );

    clientPeer->DetachPlugin( &client );
    serverPeer->DetachPlugin( &server );
}

TEST_CASE( "NatTypeDetectionClient reports NAT_TYPE_UNKNOWN when c2 fails to bind", "[nattypedetection][network]" )
{
    UnboundC2Client client;

    PeerScope peers;
    RakPeerInterface* peer = peers.Client();
    peer->AttachPlugin( &client );

    client.DetectNATType( SystemAddress( "127.0.0.1", kServerPort ) );

    CHECK( WaitForDetectionResult( peer, 0 ) == NAT_TYPE_UNKNOWN );
    CHECK_FALSE( client.InProgress() );
    CHECK( client.C2() == 0 );

    peer->DetachPlugin( &client );
}
