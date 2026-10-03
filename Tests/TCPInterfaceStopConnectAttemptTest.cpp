#include "TCPInterface.h"
#include "PluginInterface2.h"
#include "RakNetTypes.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

/*
Pins that TCPInterface::Stop waits for every connect attempt, and ends the ones still in
flight promptly.

The defect was that Stop waited for the update thread only. A connect attempt, on a
non-blocking Connect's own thread or a blocking Connect's caller, ran on past Stop and then
released or activated its connection record in the array Stop had freed. On Linux, Stop's
close of the attempt's descriptor did not even end it: connect() ran on to the SYN timeout.

The attempts go to 192.0.2.1 (TEST-NET-1), which never answers, so they stay in flight
until something ends them. A host with no route there fails them at once instead; the tests
check for that and skip, since then there is nothing in flight for Stop to end.
*/

using namespace RakNet;

namespace {

const char* const kUnansweredHost = "192.0.2.1";
constexpr unsigned short kUnansweredPort = 80;

// TCP, so it shares no space with the UDP ports the rest of the suite hardcodes, and
// distinct from the other TCPInterface tests' ports.
constexpr unsigned short kRefusalListenPort = 31048;

constexpr int kNonBlockingAttemptCount = 4;

// Long enough for each attempt to be inside its connect.
constexpr std::chrono::milliseconds kSettle( 200 );

// How long Stop may take to end attempts in flight. Far below any OS connect timeout.
constexpr std::chrono::milliseconds kStopDeadline( 2000 );

std::chrono::milliseconds TimeStop( TCPInterface& tcpInterface )
{
    const auto start = std::chrono::steady_clock::now();
    tcpInterface.Stop();
    return std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - start );
}

// Calls Connect from inside Stop, once, and records what it answered.
class ConnectOnShutdown : public PluginInterface2
{
public:
    explicit ConnectOnShutdown( TCPInterface& tcpInterface )
    : tcpInterface( tcpInterface )
    {
    }

    void OnRakPeerShutdown( void ) override
    {
        if( hasRun )
            return;
        hasRun = true;

        result = tcpInterface.Connect( "127.0.0.1", kRefusalListenPort, true );
        connectionCount = tcpInterface.GetConnectionCount();
        failedAttempt = tcpInterface.HasFailedConnectionAttempt();
    }

    TCPInterface& tcpInterface;
    bool hasRun = false;
    SystemAddress result;
    unsigned short connectionCount = 0;
    SystemAddress failedAttempt;
};

} // namespace

TEST_CASE( "TCPInterface Stop ends non-blocking connect attempts still in flight", "[tcpinterface][network]" )
{
    TCPInterface client;
    REQUIRE( client.Start( 0, 0, kNonBlockingAttemptCount ) );

    for( int i = 0; i < kNonBlockingAttemptCount; i++ )
        client.Connect( kUnansweredHost, kUnansweredPort, false );

    std::this_thread::sleep_for( kSettle );
    if( client.HasFailedConnectionAttempt() != UNASSIGNED_SYSTEM_ADDRESS )
        SKIP( "Connects to 192.0.2.1 fail at once on this host, so none is in flight for Stop to end" );

    CHECK( TimeStop( client ) < kStopDeadline );
}

TEST_CASE( "TCPInterface Stop ends a blocking connect attempt on another thread", "[tcpinterface][network]" )
{
    TCPInterface client;
    REQUIRE( client.Start( 0, 0, 1 ) );

    std::atomic<bool> isConnectDone( false );
    SystemAddress result;
    std::thread connecting( [&] {
        result = client.Connect( kUnansweredHost, kUnansweredPort, true );
        isConnectDone = true;
    } );

    std::this_thread::sleep_for( kSettle );
    if( isConnectDone )
    {
        connecting.join();
        SKIP( "Connects to 192.0.2.1 fail at once on this host, so none is in flight for Stop to end" );
    }

    CHECK( TimeStop( client ) < kStopDeadline );

    // Stop waits for the attempt, so it has ended by the time Stop returns.
    CHECK( isConnectDone );
    connecting.join();
    CHECK( result == UNASSIGNED_SYSTEM_ADDRESS );
}

TEST_CASE( "TCPInterface Connect called while Stop runs is refused", "[tcpinterface][network]" )
{
    TCPInterface server;
    REQUIRE( server.Start( kRefusalListenPort, 4 ) );

    TCPInterface client;
    ConnectOnShutdown connectOnShutdown( client );
    REQUIRE( client.Start( 0, 0, 1 ) );
    client.AttachPlugin( &connectOnShutdown );

    client.Stop();
    client.DetachPlugin( &connectOnShutdown );

    REQUIRE( connectOnShutdown.hasRun );
    CHECK( connectOnShutdown.result == UNASSIGNED_SYSTEM_ADDRESS );
    // Refused rather than failed, and no connection record was claimed for it.
    CHECK( connectOnShutdown.connectionCount == 0 );
    CHECK( connectOnShutdown.failedAttempt == UNASSIGNED_SYSTEM_ADDRESS );

    server.Stop();
}
