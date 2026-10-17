#include "PeerScope.h"

#include "ConnectionWaits.h"
#include "RakNetStringMakers.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

/*
Pins what a Reroute may do to the connection records: it moves one open connection record
to a new address, and never onto an address another active record holds.

The shapes pinned here:

- A reroute onto an address no record holds moves the connection there: its GUID resolves
  to the new address.
- A reroute onto the address of another open connection is refused: both connections keep
  their addresses, and each address still resolves to its own GUID.

RakPeerInterface functions explicitly tested:

    ChangeSystemAddress
    GetSystemAddressFromGuid
    GetGuidFromSystemAddress
*/

using namespace RakNet;

namespace {

// Ports no other test uses.
constexpr unsigned short kFreeServerPort = 31710;
constexpr unsigned short kFreeTargetPort = 31711;
constexpr unsigned short kHeldServerPort = 31720;
constexpr unsigned short kHeldOtherServerPort = 31721;

// Hang guard for the wait below, not a settle time: it normally ends within an update
// cycle or two.
constexpr TimeMS kWaitBudgetMs = 10000;

// A timeout no build uses as its default, so a match can only come from the marker.
constexpr TimeMS kMarkerTimeout = 45678;

/// Queues a setting behind whatever \a peer already has buffered and waits until the view
/// shows it on its connection to \a connected. Buffered commands are applied in order, so
/// on return every command queued before this call has been applied and published.
///
/// Found by GUID, wherever the connection's address is by then: GetTimeoutTime on an
/// address the view lacks answers with the Peer-wide default, which the setter changes at
/// once.
bool WaitUntilBufferedCommandsApplied( RakPeerInterface* peer, RakNetGUID connected )
{
    peer->SetTimeoutTime( kMarkerTimeout, UNASSIGNED_SYSTEM_ADDRESS );
    return ConnectionWaits::WaitUntil(
        [&] {
            ConnectionWaits::RunUpdateCycle( peer );
            const SystemAddress address = peer->GetSystemAddressFromGuid( connected );
            return address != UNASSIGNED_SYSTEM_ADDRESS && peer->GetTimeoutTime( address ) == kMarkerTimeout;
        },
        kWaitBudgetMs );
}

} // namespace

TEST_CASE( "A reroute onto an address no record holds moves the connection there", "[network]" )
{
    PeerScope peers;

    RakPeerInterface* a = peers.Client( 0, 1 );
    RakPeerInterface* b = peers.Server( kFreeServerPort );

    ConnectionWaits::ConnectAndWait( a, b );
    const RakNetGUID guidOfB = b->GetMyGUID();
    const SystemAddress newAddress( "127.0.0.1", kFreeTargetPort );

    a->ChangeSystemAddress( guidOfB, newAddress );
    REQUIRE( WaitUntilBufferedCommandsApplied( a, guidOfB ) );

    CHECK( a->GetSystemAddressFromGuid( guidOfB ) == newAddress );
    CHECK( a->GetGuidFromSystemAddress( newAddress ) == guidOfB );
}

TEST_CASE( "A reroute onto the address of another open connection is refused", "[network]" )
{
    PeerScope peers;

    RakPeerInterface* a = peers.Client( 0, 2 );
    RakPeerInterface* b = peers.Server( kHeldServerPort );
    RakPeerInterface* c = peers.Server( kHeldOtherServerPort );

    ConnectionWaits::ConnectAndWait( a, b );
    ConnectionWaits::ConnectAndWait( a, c );
    const RakNetGUID guidOfB = b->GetMyGUID();
    const RakNetGUID guidOfC = c->GetMyGUID();
    const SystemAddress addressOfB = a->GetSystemAddressFromGuid( guidOfB );
    const SystemAddress addressOfC = a->GetSystemAddressFromGuid( guidOfC );
    REQUIRE( addressOfB != UNASSIGNED_SYSTEM_ADDRESS );
    REQUIRE( addressOfC != UNASSIGNED_SYSTEM_ADDRESS );

    a->ChangeSystemAddress( guidOfB, addressOfC );
    REQUIRE( WaitUntilBufferedCommandsApplied( a, guidOfC ) );

    CHECK( a->GetGuidFromSystemAddress( addressOfC ) == guidOfC );
    CHECK( a->GetSystemAddressFromGuid( guidOfB ) == addressOfB );
    CHECK( a->GetGuidFromSystemAddress( addressOfB ) == guidOfB );
}
