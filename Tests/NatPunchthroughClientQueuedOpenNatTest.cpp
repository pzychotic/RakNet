#include "Plugins/NatPunchthroughClient.h"

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
#include <iterator>
#include <thread>
#include <vector>

/*
NatPunchthroughClient sends every OpenNAT it queued while the port stride was
being calculated, once each, in the order they were queued.

The regression: the std::deque port turned SendQueuedOpenNAT's FIFO Pop() into
front() followed by pop_back(). With [A, B, C] queued it sent A three times and
never sent B or C.

The facilitator is a plain peer standing in for NatPunchthroughServer, which is
all the queue needs. The first OpenNAT on a client with an unknown port stride
sends ID_NAT_REQUEST_BOUND_ADDRESSES and queues; the next two queue behind it
while the stride is being calculated. The facilitator answers with no bound
addresses, which marks the stride INCAPABLE and flushes the queue as
ID_NAT_PUNCHTHROUGH_REQUESTs - RELIABLE_ORDERED on channel 0, so the facilitator
reads them back in the order the client sent them.

The client's plugin only sees the answer from RakPeer::Receive, so both peers
are polled until the facilitator holds three requests, then for a grace period
after, so a fourth request - a destination sent twice - is read too. Update's
5 s stride timeout would flush the queue as well, but through the same function,
so it cannot fake a pass.
*/

using namespace RakNet;

namespace {

constexpr unsigned short kFacilitatorPort = 30000;

// Hang guard for the three requests. On loopback they arrive a few update
// cycles after the answer, tens of milliseconds.
constexpr TimeMS kRequestBudgetMs = 5000;

// How long to keep reading once all three are in. Every request comes out of
// one SendQueuedOpenNAT call, so a duplicate would be sent in the same update
// cycle as the rest; half a second is dozens of cycles past that.
constexpr TimeMS kDuplicateGraceMs = 500;

} // namespace

TEST_CASE( "NatPunchthroughClient sends each queued OpenNAT once and in order", "[network]" )
{
    // Before the PeerScope, so it outlives the peers: DestroyInstance calls
    // OnRakPeerShutdown on every plugin still attached, and a failing REQUIRE
    // throws past the DetachPlugin at the bottom.
    NatPunchthroughClient punch;

    PeerScope peers;
    RakPeerInterface* facilitator = peers.Server( kFacilitatorPort );
    RakPeerInterface* client = peers.Client();
    client->AttachPlugin( &punch );

    ConnectionWaits::ConnectAndWait( client, facilitator );

    const SystemAddress facilitatorAddress( "127.0.0.1", kFacilitatorPort );
    const RakNetGUID queued[] = { RakNetGUID( 1001 ), RakNetGUID( 1002 ), RakNetGUID( 1003 ) };
    for( const RakNetGUID& destination : queued )
        REQUIRE( punch.OpenNAT( destination, facilitatorAddress ) );

    // One poll of both peers. The facilitator answers the bound-address request
    // with none and records each punchthrough request; the client only needs
    // Receive to run its plugin.
    std::vector<RakNetGUID> requested;
    const auto poll = [&]() {
        for( Packet* packet = facilitator->Receive(); packet != nullptr; packet = facilitator->Receive() )
        {
            if( packet->data[0] == ID_NAT_REQUEST_BOUND_ADDRESSES )
            {
                BitStream answer;
                answer.Write( (MessageID)ID_NAT_RESPOND_BOUND_ADDRESSES );
                answer.Write( (unsigned char)0 );
                facilitator->Send( &answer, HIGH_PRIORITY, RELIABLE_ORDERED, 0, packet->systemAddress, false );
            }
            else if( packet->data[0] == ID_NAT_PUNCHTHROUGH_REQUEST )
            {
                BitStream bs( packet->data, packet->length, false );
                bs.IgnoreBytes( sizeof( MessageID ) );
                RakNetGUID destination;
                if( bs.Read( destination ) )
                    requested.push_back( destination );
            }
            facilitator->DeallocatePacket( packet );
        }
        ConnectionWaits::Drain( client );
        std::this_thread::sleep_for( std::chrono::milliseconds( ConnectionWaits::kPollInterval ) );
    };

    const TimeMS deadline = GetTimeMS() + kRequestBudgetMs;
    while( requested.size() < std::size( queued ) && !ConnectionWaits::Expired( deadline ) )
        poll();
    const TimeMS graceEnd = GetTimeMS() + kDuplicateGraceMs;
    while( !ConnectionWaits::Expired( graceEnd ) )
        poll();

    REQUIRE( requested.size() == std::size( queued ) );
    CHECK( requested[0] == queued[0] );
    CHECK( requested[1] == queued[1] );
    CHECK( requested[2] == queued[2] );

    client->DetachPlugin( &punch );
}
