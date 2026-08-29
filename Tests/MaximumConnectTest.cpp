/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "ConnectionWaits.h"
#include "PeerScope.h"

#include "RakNetTypes.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

/*
Eight peers, each started with room for four connections and an incoming limit of
four, all try to connect to one another: peer i calls Connect on every peer above
it, so all 28 pairs are attempted against 32 connection slots. No peer may end up
holding more than four connections.

RakPeerInterface functions explicitly tested:

    SetMaximumIncomingConnections
    GetMaximumIncomingConnections
    GetSystemList

Exercised indirectly by getting to that point: Startup, Connect, Receive,
DeallocatePacket, GetConnectionState.

Which peer ends up connected to which is NOT deterministic, and no assertion here
pretends otherwise. Measured over thirteen runs, the eight peers between them held
18 to 22 connection ends, distributed differently every time - hence a floor with
slack rather than a count, and a symmetry check rather than a shape.

Two limits are in play, and this test cannot tell them apart. The incoming limit
in its name is checked as GetNumberOfRemoteInitiatedConnections() <
GetMaximumIncomingConnections() (Source/RakPeer.cpp:3493), and that count only
counts connections already CONNECTED - so when eight peers fire at once over
loopback, seven requests can arrive at a peer before any of them completes and all
seven are allowed. What actually caps a peer here is Startup's slot count, set to
the same four: a peer with seven outstanding requests of its own spends its slots
holding them, its targets' acceptances are then dropped, and that is why the
low-numbered peers - the ones that initiate the most - routinely end up with the
fewest connections. Perturbing SetMaximumIncomingConnections alone does not
reliably move any assertion here; perturbing the slot count moves all of them.
*/

using namespace RakNet;

namespace {

constexpr int kPeerNum = 8;
constexpr unsigned short kBasePort = 60000;

// Both the Startup slot count and the incoming limit.
constexpr unsigned short kMaxConnections = 4;

// Counted in connection ends: one connection contributes two, one to each of the
// peers holding it. Measured at 18 to 22 over thirteen runs, so this is a floor
// with better than twice the slack rather than an expected count. Its whole job
// is to rule out the vacuous pass - every assertion above it is an upper bound,
// and a run in which not one connection was ever made satisfies all of them.
constexpr int kMinimumConnectionEnds = 8;

bool HoldsPort( const std::vector<SystemAddress>& connections, unsigned short port )
{
    for( const SystemAddress& address : connections )
    {
        if( address.GetPort() == port )
        {
            return true;
        }
    }

    return false;
}

} // namespace

TEST_CASE( "Eight peers all connecting to one another do connect, and none ends up over its connection limit", "[network]" )
{
    PeerScope peers;

    RakPeerInterface* peerList[kPeerNum];

    for( int i = 0; i < kPeerNum; i++ )
    {
        peerList[i] = peers.Server( static_cast<unsigned short>( kBasePort + i ), kMaxConnections );

        INFO( "peer " << i );
        CHECK( peerList[i]->GetMaximumIncomingConnections() == kMaxConnections );
    }

    for( int i = 0; i < kPeerNum; i++ )
    {
        // From i + 1, so a pair is attempted once rather than from both ends.
        for( int j = i + 1; j < kPeerNum; j++ )
        {
            INFO( "peer " << i << " connecting to peer " << j );
            REQUIRE( peerList[i]->Connect( "127.0.0.1", kBasePort + j, 0, 0 ) == CONNECTION_ATTEMPT_STARTED );
        }
    }

    // Every ordered pair, both directions of each attempt - see ConnectionWaits.h
    // for why the direction the test never calls Connect on is the one that
    // matters. When this returns, no further connection can form and no further
    // refusal can arrive, so the system lists below are readings rather than
    // snapshots of a race.
    ConnectionWaits::WaitForAllPairsToSettle( peerList, kPeerNum, kBasePort );

    ConnectionWaits::DrainAll( peerList, kPeerNum );

    // Read once, so every check below is against one settled state rather than
    // eight separately-timed reads of it.
    std::vector<std::vector<SystemAddress>> connectionsOf( kPeerNum );

    std::vector<SystemAddress> systemList;
    std::vector<RakNetGUID> guidList;

    for( int i = 0; i < kPeerNum; i++ )
    {
        peerList[i]->GetSystemList( systemList, guidList );
        connectionsOf[i] = systemList;
    }

    int connectionEnds = 0;

    for( int i = 0; i < kPeerNum; i++ )
    {
        const int connectionCount = static_cast<int>( connectionsOf[i].size() );
        connectionEnds += connectionCount;

        // CHECK, not REQUIRE: nothing below depends on it, and one peer over the
        // limit should not hide another.
        INFO( "peer " << i );
        CHECK( connectionCount <= kMaxConnections );
    }

    CHECK( connectionEnds >= kMinimumConnectionEnds );

    // The other half of what the wait above buys: a connection is a thing two
    // peers hold, so if peer i lists peer j then peer j lists peer i. Only
    // askable because both sides of every attempt have settled.
    for( int i = 0; i < kPeerNum; i++ )
    {
        for( int j = i + 1; j < kPeerNum; j++ )
        {
            const bool iHoldsJ = HoldsPort( connectionsOf[i], static_cast<unsigned short>( kBasePort + j ) );
            const bool jHoldsI = HoldsPort( connectionsOf[j], static_cast<unsigned short>( kBasePort + i ) );

            INFO( "peer " << i << " and peer " << j );
            CHECK( iHoldsJ == jHoldsI );
        }
    }
}
