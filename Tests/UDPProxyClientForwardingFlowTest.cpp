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

One proxy server, so the coordinator skips its ping step. With more than one, the client
misreads the coordinator's ping request (ticket 32 in .scratch/upstream-defects), so that
step cannot run end to end yet; UDPProxyClientEntitlementTest covers who may start it.
*/

using namespace RakNet;

namespace {

constexpr unsigned short kCoordinatorPort = 60000;
constexpr unsigned short kProxyServerPort = 60001;
constexpr unsigned short kSourcePort = 60002;
constexpr unsigned short kTargetPort = 60003;

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
