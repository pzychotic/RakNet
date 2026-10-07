#include "Plugins/Router2.h"

#include "BitStream.h"
#include "ConnectionWaits.h"
#include "MarkerInjection.h"
#include "MessageIdentifiers.h"
#include "PeerScope.h"
#include "RakNetStringMakers.h"
#include "RakNetTypes.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

/*
Router2 consumes an ID_ROUTER_2_FORWARDING_ESTABLISHED naming an endpoint it never asked to
reach.

OnForwardingSuccess took a miss in forwardedConnectionList to mean a connection request was
pending, and indexed connectionRequests with whatever GetConnectionRequestIndex returned -
~0u when there was none - then erased at that index. Any connected System could trigger it
with one Message. Under MSVC debug iterators that aborts; elsewhere it is undefined
behaviour.

The forged Message is checked through MarkerInjection.
*/

using namespace RakNet;
using MarkerInjection::Contains;

namespace {

constexpr unsigned short kServerPort = 30000;

} // namespace

TEST_CASE( "Router2 consumes a forwarding success for an endpoint it never asked to reach", "[router2][network]" )
{
    // Before the PeerScope, so it outlives the peers.
    Router2 router;

    PeerScope peers;
    RakPeerInterface* server = peers.Server( kServerPort );
    RakPeerInterface* client = peers.Client();
    server->AttachPlugin( &router );

    ConnectionWaits::ConnectAndWait( client, server );

    const SystemAddress clientAddressBefore = server->GetSystemAddressFromGuid( client->GetMyGUID() );

    BitStream forged;
    forged.Write( (MessageID)ID_ROUTER_2_FORWARDING_ESTABLISHED );
    forged.Write( RakNetGUID( 1001 ) );
    forged.Write( (unsigned short)25000 );
    const std::vector<MessageID> received = MarkerInjection::Inject( client, server, forged );

    CHECK( !Contains( received, ID_ROUTER_2_FORWARDING_ESTABLISHED ) );
    CHECK( !Contains( received, ID_ROUTER_2_REROUTED ) );
    CHECK( server->GetSystemAddressFromGuid( client->GetMyGUID() ) == clientAddressBefore );
    CHECK( server->GetSystemAddressFromGuid( RakNetGUID( 1001 ) ) == UNASSIGNED_SYSTEM_ADDRESS );

    server->DetachPlugin( &router );
}
