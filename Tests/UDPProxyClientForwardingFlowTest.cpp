#include "Plugins/UDPProxyClient.h"
#include "Plugins/UDPProxyCoordinator.h"
#include "Plugins/UDPProxyServer.h"

#include "ConnectionWaits.h"
#include "GetTime.h"
#include "MessageIdentifiers.h"
#include "PeerScope.h"
#include "RakNetStringMakers.h"
#include "RakNetTypes.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

/*
The UDPProxy flow, end to end on loopback: a source asks a coordinator for forwarding to a
target, the coordinator has its one proxy server set it up, and both ends hear about it.

The source's result is Solicited and needs nothing designated. The target's notification
reaches it only if it designated the coordinator, and without it the target never pings the
proxy, which is what opens a NAT in front of it.

With one proxy server the coordinator skips its ping step. With two, it first has both ends
ping every server and chooses by their replies. Stock's client never read the target's
RakNetGUID in the ping request, so it took part of the GUID as the server count and pinged
the wrong addresses; the coordinator dropped every reply and fell back to list order after
DEFAULT_UNRESPONSIVE_PING_TIME_COORDINATOR. UDPProxyClientEntitlementTest covers who may
start the ping step.
*/

using namespace RakNet;

namespace {

constexpr unsigned short kCoordinatorPort = 30000;
constexpr unsigned short kProxyServerPort = 30001;
constexpr unsigned short kSourcePort = 30002;
constexpr unsigned short kTargetPort = 30003;
constexpr unsigned short kSecondProxyServerPort = 30004;

// Hang guard for each step. On loopback each takes a few update cycles, tens of
// milliseconds.
constexpr TimeMS kStepBudgetMs = 10000;

// How long the test watches for a notification that must not come, after the source has
// its result. The coordinator sends both in the same update.
constexpr TimeMS kNoNotificationWindowMs = 300;

constexpr TimeMS kForwardingTimeoutMs = 10000;

const char* const kPassword = "password";

struct LoginHandler : public UDPProxyServerResultHandler
{
    bool loggedIn = false;

    void OnLoginSuccess( const std::string&, UDPProxyServer* ) override { loggedIn = true; }
    void OnAlreadyLoggedIn( const std::string&, UDPProxyServer* ) override {}
    void OnNoPasswordSet( const std::string&, UDPProxyServer* ) override {}
    void OnWrongPassword( const std::string&, UDPProxyServer* ) override {}
};

struct ForwardingHandler : public UDPProxyClientResultHandler
{
    int successes = 0;
    int notifications = 0;
    int failures = 0;
    unsigned short proxyPort = 0;

    void OnForwardingSuccess( const char*, unsigned short port, SystemAddress, SystemAddress, SystemAddress, RakNetGUID, UDPProxyClient* ) override
    {
        successes++;
        proxyPort = port;
    }
    void OnForwardingNotification( const char*, unsigned short port, SystemAddress, SystemAddress, SystemAddress, RakNetGUID, UDPProxyClient* ) override
    {
        notifications++;
        proxyPort = port;
    }
    void OnNoServersOnline( SystemAddress, SystemAddress, SystemAddress, RakNetGUID, UDPProxyClient* ) override { failures++; }
    void OnRecipientNotConnected( SystemAddress, SystemAddress, SystemAddress, RakNetGUID, UDPProxyClient* ) override { failures++; }
    void OnAllServersBusy( SystemAddress, SystemAddress, SystemAddress, RakNetGUID, UDPProxyClient* ) override { failures++; }
    void OnForwardingInProgress( const char*, unsigned short, SystemAddress, SystemAddress, SystemAddress, RakNetGUID, UDPProxyClient* ) override { failures++; }
};

// Receives on every peer, since the plugins run inside Receive, until done() holds or budget
// passes. Returns done().
bool PumpUntil( const std::vector<RakPeerInterface*>& peers, const std::function<bool()>& done, TimeMS budget = kStepBudgetMs )
{
    const TimeMS deadline = GetTimeMS() + budget;
    while( !done() && !ConnectionWaits::Expired( deadline ) )
    {
        ConnectionWaits::DrainAll( peers.data(), (int)peers.size() );
        std::this_thread::sleep_for( std::chrono::milliseconds( ConnectionWaits::kPollInterval ) );
    }
    return done();
}

// Receives on every peer until pinger's Receive hands out a pong from answerer, or the
// budget passes.
bool PongFrom( const std::vector<RakPeerInterface*>& peers, RakPeerInterface* pinger, RakNetGUID answerer )
{
    const TimeMS deadline = GetTimeMS() + kStepBudgetMs;
    while( !ConnectionWaits::Expired( deadline ) )
    {
        for( RakPeerInterface* peer : peers )
        {
            for( Packet* packet = peer->Receive(); packet != nullptr; packet = peer->Receive() )
            {
                const bool pong = peer == pinger && packet->data[0] == ID_UNCONNECTED_PONG && packet->guid == answerer;
                peer->DeallocatePacket( packet );
                if( pong )
                    return true;
            }
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( ConnectionWaits::kPollInterval ) );
    }
    return false;
}

// Records which servers each end's ping reply named, right after the coordinator takes it.
// The forwarding request is gone once forwarding succeeds, so it cannot be looked at later.
class CoordinatorProbe : public UDPProxyCoordinator
{
public:
    std::vector<SystemAddress> sourcePinged, targetPinged;

    PluginReceiveResult OnReceive( Packet* packet ) override
    {
        const bool pingReply = packet->length > 1 && packet->data[0] == ID_UDP_PROXY_GENERAL &&
                               packet->data[1] == ID_UDP_PROXY_PING_SERVERS_REPLY_FROM_CLIENT_TO_COORDINATOR;
        const PluginReceiveResult result = UDPProxyCoordinator::OnReceive( packet );
        // The test makes one request
        if( pingReply && forwardingRequestList.Size() == 1 )
        {
            sourcePinged = Addresses( forwardingRequestList[0]->sourceServerPings );
            targetPinged = Addresses( forwardingRequestList[0]->targetServerPings );
        }
        return result;
    }

private:
    static std::vector<SystemAddress> Addresses( const std::vector<ServerWithPing>& pings )
    {
        std::vector<SystemAddress> addresses;
        for( const ServerWithPing& swp : pings )
            addresses.push_back( swp.serverAddress );
        // Every server is on loopback, so the port alone orders them
        std::sort( addresses.begin(), addresses.end(), []( const SystemAddress& a, const SystemAddress& b ) { return a.GetPort() < b.GetPort(); } );
        return addresses;
    }
};

void Connect( const std::vector<RakPeerInterface*>& peers, RakPeerInterface* client )
{
    REQUIRE( client->Connect( "127.0.0.1", kCoordinatorPort, nullptr, 0 ) == CONNECTION_ATTEMPT_STARTED );
    REQUIRE( PumpUntil( peers, [client]() { return client->GetConnectionState( SystemAddress( "127.0.0.1", kCoordinatorPort ) ) == IS_CONNECTED; } ) );
}

} // namespace

TEST_CASE( "UDPProxyClient's forwarding flow reaches a target that designated its coordinator", "[udpproxy][network]" )
{
    // Before the PeerScope, so they outlive the peers.
    UDPProxyCoordinator coordinatorPlugin;
    UDPProxyServer proxyServerPlugin;
    UDPProxyClient sourcePlugin, targetPlugin;
    LoginHandler loginHandler;
    ForwardingHandler sourceHandler, targetHandler;
    coordinatorPlugin.SetRemoteLoginPassword( kPassword );
    proxyServerPlugin.SetResultHandler( &loginHandler );
    sourcePlugin.SetResultHandler( &sourceHandler );
    targetPlugin.SetResultHandler( &targetHandler );

    PeerScope scope;
    RakPeerInterface* coordinator = scope.Server( kCoordinatorPort, 4 );
    RakPeerInterface* proxyServer = scope.Client( kProxyServerPort );
    RakPeerInterface* source = scope.Client( kSourcePort );
    RakPeerInterface* target = scope.Client( kTargetPort );
    coordinator->AttachPlugin( &coordinatorPlugin );
    proxyServer->AttachPlugin( &proxyServerPlugin );
    source->AttachPlugin( &sourcePlugin );
    target->AttachPlugin( &targetPlugin );
    const std::vector<RakPeerInterface*> peers{ coordinator, proxyServer, source, target };

    const SystemAddress coordinatorAddress( "127.0.0.1", kCoordinatorPort );
    Connect( peers, proxyServer );
    Connect( peers, source );
    Connect( peers, target );

    REQUIRE( proxyServerPlugin.LoginToCoordinator( kPassword, coordinatorAddress ) );
    REQUIRE( PumpUntil( peers, [&]() { return loginHandler.loggedIn; } ) );

    SECTION( "Both ends hear of it when both designated the coordinator" )
    {
        sourcePlugin.AddCoordinator( coordinatorAddress );
        targetPlugin.AddCoordinator( coordinatorAddress );

        REQUIRE( sourcePlugin.RequestForwarding( coordinatorAddress, UNASSIGNED_SYSTEM_ADDRESS, target->GetMyGUID(), kForwardingTimeoutMs ) );
        REQUIRE( PumpUntil( peers, [&]() { return sourceHandler.successes == 1 && targetHandler.notifications == 1; } ) );
        CHECK( sourceHandler.failures == 0 );
        CHECK( sourceHandler.proxyPort == targetHandler.proxyPort );

        // The proxy forwards: a ping from the source through it is answered by the target.
        source->Ping( "127.0.0.1", sourceHandler.proxyPort, false );
        CHECK( PongFrom( peers, source, target->GetMyGUID() ) );
    }

    SECTION( "The source hears of it without designating, and an undesignating target does not" )
    {
        REQUIRE( sourcePlugin.RequestForwarding( coordinatorAddress, UNASSIGNED_SYSTEM_ADDRESS, target->GetMyGUID(), kForwardingTimeoutMs ) );
        REQUIRE( PumpUntil( peers, [&]() { return sourceHandler.successes == 1; } ) );
        CHECK( !PumpUntil( peers, [&]() { return targetHandler.notifications != 0; }, kNoNotificationWindowMs ) );
        CHECK( sourceHandler.failures == 0 );
    }

    source->DetachPlugin( &sourcePlugin );
    target->DetachPlugin( &targetPlugin );
    proxyServer->DetachPlugin( &proxyServerPlugin );
    coordinator->DetachPlugin( &coordinatorPlugin );
}

TEST_CASE( "UDPProxyClient's ping replies reach the coordinator when two proxy servers are logged in", "[udpproxy][network]" )
{
    CoordinatorProbe coordinatorPlugin;
    UDPProxyServer firstServerPlugin, secondServerPlugin;
    UDPProxyClient sourcePlugin, targetPlugin;
    LoginHandler firstLogin, secondLogin;
    ForwardingHandler sourceHandler, targetHandler;
    coordinatorPlugin.SetRemoteLoginPassword( kPassword );
    firstServerPlugin.SetResultHandler( &firstLogin );
    secondServerPlugin.SetResultHandler( &secondLogin );
    sourcePlugin.SetResultHandler( &sourceHandler );
    targetPlugin.SetResultHandler( &targetHandler );

    PeerScope scope;
    RakPeerInterface* coordinator = scope.Server( kCoordinatorPort, 5 );
    RakPeerInterface* firstServer = scope.Client( kProxyServerPort );
    RakPeerInterface* secondServer = scope.Client( kSecondProxyServerPort );
    RakPeerInterface* source = scope.Client( kSourcePort );
    RakPeerInterface* target = scope.Client( kTargetPort );
    coordinator->AttachPlugin( &coordinatorPlugin );
    firstServer->AttachPlugin( &firstServerPlugin );
    secondServer->AttachPlugin( &secondServerPlugin );
    source->AttachPlugin( &sourcePlugin );
    target->AttachPlugin( &targetPlugin );
    const std::vector<RakPeerInterface*> peers{ coordinator, firstServer, secondServer, source, target };

    const SystemAddress coordinatorAddress( "127.0.0.1", kCoordinatorPort );
    Connect( peers, firstServer );
    Connect( peers, secondServer );
    Connect( peers, source );
    Connect( peers, target );

    REQUIRE( firstServerPlugin.LoginToCoordinator( kPassword, coordinatorAddress ) );
    REQUIRE( secondServerPlugin.LoginToCoordinator( kPassword, coordinatorAddress ) );
    REQUIRE( PumpUntil( peers, [&]() { return firstLogin.loggedIn && secondLogin.loggedIn; } ) );

    sourcePlugin.AddCoordinator( coordinatorAddress );
    targetPlugin.AddCoordinator( coordinatorAddress );

    REQUIRE( sourcePlugin.RequestForwarding( coordinatorAddress, UNASSIGNED_SYSTEM_ADDRESS, target->GetMyGUID(), kForwardingTimeoutMs ) );
    REQUIRE( PumpUntil( peers, [&]() { return sourceHandler.successes == 1 && targetHandler.notifications == 1; } ) );
    CHECK( sourceHandler.failures == 0 );

    // Which server wins is not asserted: loopback pings give no meaningful order.
    const std::vector<SystemAddress> bothServers{ SystemAddress( "127.0.0.1", kProxyServerPort ), SystemAddress( "127.0.0.1", kSecondProxyServerPort ) };
    CHECK( coordinatorPlugin.sourcePinged == bothServers );
    CHECK( coordinatorPlugin.targetPinged == bothServers );

    source->DetachPlugin( &sourcePlugin );
    target->DetachPlugin( &targetPlugin );
    secondServer->DetachPlugin( &secondServerPlugin );
    firstServer->DetachPlugin( &firstServerPlugin );
    coordinator->DetachPlugin( &coordinatorPlugin );
}
