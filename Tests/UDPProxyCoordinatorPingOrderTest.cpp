#include "Plugins/UDPProxyCoordinator.h"
#include "Plugins/UDPProxyCommon.h"

#include "BitStream.h"
#include "MessageIdentifiers.h"
#include "RakMemoryOverride.h"
#include "RakNetStringMakers.h"
#include "RakNetTypes.h"
#include "UDPProxyWire.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

/*
UDPProxyCoordinator orders the proxy servers it will try by each server's own summed ping,
and a ping reply cannot make it read past a ping list.

OrderRemainingServersToTry read sourceServerPings[idx] and targetServerPings[idx] for every
server it was asked about, so a client replying with fewer servers made it read past the end
of a vector. Under MSVC debug iterators that aborts; elsewhere it is undefined behaviour. Even
with matching sizes the index was wrong: the ping lists are filled from the wire in ping
order, not server order, so a server was scored with other servers' pings.

The reply handler also took every address in the reply, as often as it appeared, so one
reply could grow a ping list by 65535 entries and a repeated reply grew it again.
*/

using namespace RakNet;
using namespace UDPProxyWire;

namespace {

using ServerWithPing = UDPProxyCoordinator::ServerWithPing;
using ForwardingRequest = UDPProxyCoordinator::ForwardingRequest;

const SystemAddress kServerA( "10.0.0.1", 1001 );
const SystemAddress kServerB( "10.0.0.2", 1002 );
const SystemAddress kServerC( "10.0.0.3", 1003 );
const SystemAddress kUnknownServer( "10.0.0.9", 1009 );
const SystemAddress kSourceClient( "10.0.1.1", 2001 );
const SystemAddress kTargetClient( "10.0.1.2", 2002 );

std::vector<SystemAddress> RemainingServers( const ForwardingRequest& fw )
{
    return std::vector<SystemAddress>( fw.remainingServersToTry.begin(), fw.remainingServersToTry.end() );
}

// The ping recorded for serverAddress, or -1 when there is none.
int PingFor( const std::vector<ServerWithPing>& pings, const SystemAddress& serverAddress )
{
    for( const ServerWithPing& swp : pings )
    {
        if( swp.serverAddress == serverAddress )
            return swp.ping;
    }
    return -1;
}

// Reaches the protected reply handler and the forwarding request list, so a reply can be
// fed straight in without two clients and two logged-in proxy servers.
class CoordinatorProbe : public UDPProxyCoordinator
{
public:
    ForwardingRequest* AddRequestAwaitingPings( const std::vector<SystemAddress>& servers )
    {
        ForwardingRequest* fw = RakNet::OP_NEW<ForwardingRequest>( _FILE_AND_LINE_ );
        fw->timeoutOnNoDataMS = 0;
        fw->timeoutAfterSuccess = 0;
        fw->sata.senderClientAddress = kSourceClient;
        fw->sata.targetClientAddress = kTargetClient;
        fw->timeRequestedPings = 1;
        fw->remainingServersToTry.assign( servers.begin(), servers.end() );
        bool objectExists;
        unsigned int index = forwardingRequestList.GetIndexFromKey( fw->sata, &objectExists );
        forwardingRequestList.InsertAtIndex( fw, index, _FILE_AND_LINE_ );
        return fw;
    }

    void ReceivePingReply( const SystemAddress& from, const std::vector<ServerWithPing>& pings )
    {
        BitStream reply;
        WritePingReply( reply, kSourceClient, kTargetClient, pings );

        Packet packet{};
        packet.systemAddress = from;
        packet.data = reply.GetData();
        packet.length = reply.GetNumberOfBytesUsed();
        packet.bitSize = reply.GetNumberOfBitsUsed();
        OnPingServersReplyFromClientToCoordinator( &packet );
    }
};

} // namespace

TEST_CASE( "A short ping reply orders the servers without reading past it", "[UDPProxyCoordinator]" )
{
    ForwardingRequest fw;
    fw.remainingServersToTry = { kServerA, kServerB, kServerC };
    // The source client answered for one server of three.
    fw.sourceServerPings = { ServerPing( kServerC, 10 ) };
    fw.targetServerPings = { ServerPing( kServerB, 50 ), ServerPing( kServerC, 60 ), ServerPing( kServerA, 70 ) };

    fw.OrderRemainingServersToTry();

    // C: 10 + 60. B: unresponsive + 50. A: unresponsive + 70.
    CHECK( RemainingServers( fw ) == std::vector<SystemAddress>{ kServerC, kServerB, kServerA } );
}

TEST_CASE( "Servers are tried in order of their own summed ping", "[UDPProxyCoordinator]" )
{
    ForwardingRequest fw;
    fw.remainingServersToTry = { kServerA, kServerB, kServerC };
    // In ping order, as the reply handler used to keep them, which is not server order.
    fw.sourceServerPings = { ServerPing( kServerB, 10 ), ServerPing( kServerC, 100 ), ServerPing( kServerA, 300 ) };
    fw.targetServerPings = { ServerPing( kServerB, 20 ), ServerPing( kServerC, 200 ), ServerPing( kServerA, 400 ) };

    fw.OrderRemainingServersToTry();

    CHECK( RemainingServers( fw ) == std::vector<SystemAddress>{ kServerB, kServerC, kServerA } );
}

TEST_CASE( "A ping reply only records servers the coordinator asked about, once each", "[UDPProxyCoordinator]" )
{
    CoordinatorProbe coordinator;
    ForwardingRequest* fw = coordinator.AddRequestAwaitingPings( { kServerA, kServerB } );

    coordinator.ReceivePingReply( kSourceClient, { ServerPing( kUnknownServer, 1 ), ServerPing( kServerB, 30 ), ServerPing( kServerA, 20 ), ServerPing( kServerB, 5 ) } );
    // A repeated reply replaces what the first one said rather than adding to it.
    coordinator.ReceivePingReply( kSourceClient, { ServerPing( kServerA, 40 ) } );

    // The target has not answered, so the request is still waiting and nothing was tried.
    REQUIRE( fw->targetServerPings.empty() );
    REQUIRE( fw->sourceServerPings.size() == 2 );
    CHECK( PingFor( fw->sourceServerPings, kServerA ) == 40 );
    CHECK( PingFor( fw->sourceServerPings, kServerB ) == 5 );
}
