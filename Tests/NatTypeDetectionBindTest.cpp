#include "Plugins/NatTypeDetectionClient.h"
#include "Plugins/NatTypeDetectionServer.h"

#include "PeerScope.h"
#include "RakNetSocket2.h"
#include "RakNetTypes.h"
#include "RakPeerInterface.h"
#include "SocketDefines.h"
#include "SocketIncludes.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <vector>

/*
NatTypeDetectionServer binds s1p2, and NatTypeDetectionClient binds c2, on the same host as
the peer's first socket. They used to take that host from GetBoundAddress, which reports
127.0.0.1 for a wildcard-bound socket, so a peer started the default way got plugin sockets
that could only talk to the local machine. The plugin socket must be bound to what the
peer's socket is really bound to: INADDR_ANY for a wildcard peer, and the same address for a
peer bound to one.
*/

using namespace RakNet;

namespace
{

class ExposedServer : public NatTypeDetectionServer
{
public:
    RakNetSocket2* S1p2() const { return s1p2; }
};

class ExposedClient : public NatTypeDetectionClient
{
public:
    RakNetSocket2* C2() const { return c2; }
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
