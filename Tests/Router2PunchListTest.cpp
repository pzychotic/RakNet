#include "Plugins/Router2.h"

#include "BitStream.h"
#include "ConnectionWaits.h"
#include "GetTime.h"
#include "MessageIdentifiers.h"
#include "PeerScope.h"
#include "PluginInterface2.h"
#include "RakNetTypes.h"
#include "RakPeerInterface.h"
#include "SocketIncludes.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/*
Router2 drops punches from its list in three places: when a punch times out in Update, when a
System in it disconnects, and when both ends have answered it. Each drop sends something, and
the punches left keep their order.

The punches are written into the list directly. Every one names this peer as the System to
tell, so what Router2 sends comes back through the loopback, where a plugin records it in
order.
*/

using namespace RakNet;

namespace {

// Router2's internal sub-identifier for a forwarding reply.
constexpr unsigned char kReplyForwarding = 1;

// Addresses the punches name. No System listens on them.
constexpr unsigned short kFirstPunchPort = 41000;
constexpr unsigned short kPunchPortCount = 100;

// Hang guard for the clock to pass 0, which takes at most a few milliseconds.
constexpr TimeMS kClockStartBudgetMs = 1000;

// Writes punches into the list and reads it back.
class Router2Probe : public Router2
{
public:
    void AddPunch( RakNetGUID sourceGuid, RakNetGUID endpointGuid, unsigned short port, TimeMS timeout, TimeMS nextAction, bool gotReplyFromEndpoint = false )
    {
        MiniPunchRequest punch;
        punch.endpointGuid = endpointGuid;
        punch.endpointAddress = SystemAddress( "127.0.0.1", port + kPunchPortCount / 2 );
        punch.gotReplyFromEndpoint = gotReplyFromEndpoint;
        punch.sourceGuid = sourceGuid;
        punch.sourceAddress = SystemAddress( "127.0.0.1", port );
        punch.gotReplyFromSource = false;
        punch.timeout = timeout;
        punch.nextAction = nextAction;
        punch.forwardingPort = port;
        punch.forwardingSocket = INVALID_SOCKET;
        punch.createdEntry = false;
        miniPunchesInProgress.push_back( punch );
    }

    std::vector<unsigned short> PunchPorts() const
    {
        std::vector<unsigned short> ports;
        for( const MiniPunchRequest& punch : miniPunchesInProgress )
            ports.push_back( punch.forwardingPort );
        return ports;
    }

    const MiniPunchRequest& Punch( size_t index ) const { return miniPunchesInProgress[index]; }

    void ReplyBounceFrom( RakNetGUID guid )
    {
        Packet packet{};
        packet.guid = guid;
        OnMiniPunchReplyBounce( &packet );
    }
};

// Records, in order, the no-route replies and reroutes Router2 sends this peer, and the
// out-of-band messages it sends the punches' addresses.
class SendLog : public PluginInterface2
{
public:
    bool UsesReliabilityLayer( void ) const override { return true; }

    void OnDirectSocketSend( const char* data, const BitSize_t bitsUsed, SystemAddress remoteSystemAddress ) override
    {
        (void)data;
        (void)bitsUsed;
        const unsigned short port = remoteSystemAddress.GetPort();
        if( port >= kFirstPunchPort && port < kFirstPunchPort + kPunchPortCount )
            Record( "out of band to " + std::to_string( port ) );
    }

    void OnPushBackPacket( const char* data, const BitSize_t bitsUsed, SystemAddress remoteSystemAddress ) override
    {
        (void)remoteSystemAddress;
        const unsigned int length = (unsigned int)BitsToBytes( bitsUsed );
        BitStream bs( (unsigned char*)data, length, false );
        // Recorded rather than REQUIREd: this runs inside RakPeer, which a throw must not cross.
        MessageID id = 0;
        if( !bs.Read( id ) )
            return;
        if( id == ID_ROUTER_2_INTERNAL && length > 1 && (unsigned char)data[1] == kReplyForwarding )
        {
            bs.IgnoreBytes( 1 );
            RakNetGUID endpointGuid;
            if( bs.Read( endpointGuid ) )
                Record( "no route to " + std::to_string( endpointGuid.g ) );
            else
                Record( "short no-route" );
        }
        else if( id == ID_ROUTER_2_REROUTED )
        {
            RakNetGUID sourceGuid;
            unsigned short forwardingPort = 0;
            if( bs.Read( sourceGuid ) && bs.Read( forwardingPort ) )
                Record( "rerouted through " + std::to_string( forwardingPort ) );
            else
                Record( "short reroute" );
        }
    }

    std::vector<std::string> Events()
    {
        std::lock_guard<std::mutex> guard( mutex );
        return events;
    }

private:
    void Record( const std::string& event )
    {
        std::lock_guard<std::mutex> guard( mutex );
        events.push_back( event );
    }

    std::mutex mutex;
    std::vector<std::string> events;
};

// A peer connected to nothing, with the log attached ahead of Startup as a plugin that uses
// the reliability layer must be.
RakPeerInterface* StartLogged( PeerScope& scope, SendLog& log, Router2Probe& router2 )
{
    RakPeerInterface* peer = scope.Create();
    peer->AttachPlugin( &log );
    SocketDescriptor socketDescriptor( 0, 0 );
    REQUIRE( peer->Startup( 1, &socketDescriptor, 1 ) == RAKNET_STARTED );
    peer->AttachPlugin( &router2 );
    return peer;
}

} // namespace

TEST_CASE( "Router2 drops the punches that timed out and punches again on those due", "[router2]" )
{
    SendLog log;
    Router2Probe router2;
    PeerScope scope;
    RakPeerInterface* peer = StartLogged( scope, log, router2 );
    const RakNetGUID me = peer->GetMyGUID();

    // A punch with timeout 0 has timed out, and one with nextAction 0 is due, once the clock
    // has passed 0.
    const TimeMS deadline = GetTimeMS() + kClockStartBudgetMs;
    while( GetTimeMS() < 2 && !ConnectionWaits::Expired( deadline ) )
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    REQUIRE( GetTimeMS() >= 2 );
    const TimeMS later = GetTimeMS() + 1000000;

    router2.AddPunch( me, RakNetGUID( 11 ), kFirstPunchPort + 1, 0, later );
    router2.AddPunch( me, RakNetGUID( 12 ), kFirstPunchPort + 2, later, 0 );
    router2.AddPunch( me, RakNetGUID( 13 ), kFirstPunchPort + 3, 0, 0 );
    router2.AddPunch( me, RakNetGUID( 14 ), kFirstPunchPort + 4, later, later );
    router2.AddPunch( me, RakNetGUID( 15 ), kFirstPunchPort + 5, later, 0 );

    router2.Update();

    CHECK( log.Events() == std::vector<std::string>{
                               "no route to 11",
                               "out of band to 41002",
                               "out of band to 41052",
                               "no route to 13",
                               "out of band to 41005",
                               "out of band to 41055",
                           } );
    REQUIRE( router2.PunchPorts() == std::vector<unsigned short>{ kFirstPunchPort + 2, kFirstPunchPort + 4, kFirstPunchPort + 5 } );
    CHECK( router2.Punch( 0 ).nextAction != 0 );
    CHECK( router2.Punch( 1 ).nextAction == later );
    CHECK( router2.Punch( 2 ).nextAction != 0 );

    peer->DetachPlugin( &router2 );
}

TEST_CASE( "Router2 drops the punches of a System that disconnects", "[router2]" )
{
    SendLog log;
    Router2Probe router2;
    PeerScope scope;
    RakPeerInterface* peer = StartLogged( scope, log, router2 );
    const RakNetGUID me = peer->GetMyGUID();
    const RakNetGUID closed( 50 );
    const TimeMS later = GetTimeMS() + 1000000;

    // Only a source still connected hears that its punch failed.
    router2.AddPunch( me, closed, kFirstPunchPort + 1, later, later );
    router2.AddPunch( closed, RakNetGUID( 21 ), kFirstPunchPort + 2, later, later );
    router2.AddPunch( me, RakNetGUID( 22 ), kFirstPunchPort + 3, later, later );
    router2.AddPunch( me, closed, kFirstPunchPort + 4, later, later );
    router2.AddPunch( me, RakNetGUID( 23 ), kFirstPunchPort + 5, later, later );

    router2.OnClosedConnection( UNASSIGNED_SYSTEM_ADDRESS, closed, LCR_CONNECTION_LOST );

    CHECK( log.Events() == std::vector<std::string>{ "no route to 50", "no route to 50" } );
    CHECK( router2.PunchPorts() == std::vector<unsigned short>{ kFirstPunchPort + 3, kFirstPunchPort + 5 } );

    peer->DetachPlugin( &router2 );
}

TEST_CASE( "Router2 drops the punches both ends have answered", "[router2]" )
{
    SendLog log;
    Router2Probe router2;
    PeerScope scope;
    RakPeerInterface* peer = StartLogged( scope, log, router2 );
    const RakNetGUID me = peer->GetMyGUID();
    const RakNetGUID answering( 60 );
    const TimeMS later = GetTimeMS() + 1000000;

    // Each punch names this peer as its endpoint, which is told of the reroute.
    router2.AddPunch( answering, me, kFirstPunchPort + 1, later, later, true );
    router2.AddPunch( answering, me, kFirstPunchPort + 2, later, later, false );
    router2.AddPunch( RakNetGUID( 70 ), me, kFirstPunchPort + 3, later, later, true );
    router2.AddPunch( answering, me, kFirstPunchPort + 4, later, later, true );

    router2.ReplyBounceFrom( answering );

    CHECK( log.Events() == std::vector<std::string>{ "rerouted through 41001", "rerouted through 41004" } );
    REQUIRE( router2.PunchPorts() == std::vector<unsigned short>{ kFirstPunchPort + 2, kFirstPunchPort + 3 } );
    CHECK( router2.Punch( 0 ).gotReplyFromSource );
    CHECK_FALSE( router2.Punch( 1 ).gotReplyFromSource );

    peer->DetachPlugin( &router2 );
}
