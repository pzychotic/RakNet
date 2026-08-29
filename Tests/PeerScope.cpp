#include "PeerScope.h"

#include "RakNetTypes.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace RakNet;

PeerScope::~PeerScope()
{
    // Reverse creation order: clients before the server they connected to,
    // mirroring how the tests build them up. Measured, DestroyInstance releases
    // the port before it returns, even for peers still connected mid-flight, so
    // there is no wait and no Shutdown() here.
    for( auto it = m_peers.rbegin(); it != m_peers.rend(); ++it )
    {
        RakPeerInterface::DestroyInstance( *it );
    }

    m_peers.clear();
}

RakPeerInterface* PeerScope::Create()
{
    RakPeerInterface* peer = RakPeerInterface::GetInstance();
    m_peers.push_back( peer );

    return peer;
}

void PeerScope::Start( RakPeerInterface* peer, const char* caller, unsigned short port, unsigned int maxConnections )
{
    SocketDescriptor socketDescriptor( port, 0 );

    // Scoped to the enclosing test case; printed only if something below fails.
    INFO( caller << " binding port " << port );
    REQUIRE( peer->Startup( maxConnections, &socketDescriptor, 1 ) == RAKNET_STARTED );
}

RakPeerInterface* PeerScope::Server( unsigned short port, unsigned int maxConnections )
{
    RakPeerInterface* server = Create();

    Start( server, "PeerScope::Server", port, maxConnections );
    server->SetMaximumIncomingConnections( static_cast<unsigned short>( maxConnections ) );

    return server;
}

RakPeerInterface* PeerScope::Client( unsigned short port, unsigned int maxConnections )
{
    RakPeerInterface* client = Create();

    Start( client, "PeerScope::Client", port, maxConnections );

    return client;
}

void PeerScope::ReplaceWithClient( RakPeerInterface*& slot, unsigned short port, unsigned int maxConnections )
{
    const auto tracked = std::find( m_peers.begin(), m_peers.end(), slot );

    // Replacing a peer this scope does not own would leave the caller holding a
    // pointer nothing destroys, so it is a test bug, not a silent no-op.
    REQUIRE( tracked != m_peers.end() );

    RakPeerInterface::DestroyInstance( *tracked );

    // In place rather than push_back, so the peer keeps its position in the
    // reverse-order teardown.
    *tracked = RakPeerInterface::GetInstance();
    slot = *tracked;

    Start( slot, "PeerScope::ReplaceWithClient", port, maxConnections );
}
