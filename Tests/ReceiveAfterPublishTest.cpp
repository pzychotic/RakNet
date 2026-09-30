#include "PeerScope.h"
#include "RawSystem.h"

#include "ConnectionWaits.h"
#include "GetTime.h"
#include "MessageIdentifiers.h"
#include "PluginInterface2.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>

/*
Pins that a Message the network thread produces reaches Receive only once the published
view reflects the update cycle that produced it (ADR-0007, point 7).

The window this closes is the rest of the cycle after the push, which is normally well
under a millisecond. The test widens it. The network thread goes through its connection
records in the order they were created, and pushes a connection change while it handles
that record. Records created after it are handled later in the same cycle. So the System
whose connection changes is a RawSystem, driven one handshake step at a time, and two
real clients connect after its record exists. The server keeps sending to them, and a
plugin stalls the network thread when it sends to one of them, once per cycle. Every
connection change of the RawSystem is then followed, in its cycle, by a stall before the
view is published. Two clients rather than one, because closing a record moves the last
one into its place, and the loop does not come back to that place in the same cycle.

In each handler the view must already show the change:

- On ID_NEW_INCOMING_CONNECTION, the System is IS_CONNECTED, its RakNetGUID is the one
  the Message carries, and it has a ping.
- On ID_CONNECTION_LOST and ID_DISCONNECTION_NOTIFICATION, it is IS_NOT_CONNECTED.

ID_CONNECTION_REQUEST_ACCEPTED is pushed the same way on the connecting side, and is not
driven here: no RawSystem plays a server.

RakPeerInterface functions explicitly tested:

    Receive
*/

using namespace RakNet;
using RawSystemHarness::RawSystem;
using RawSystemHarness::WinsockFixture;

namespace {

// Hang guard for each wait, not a settle time: each normally ends within a second.
constexpr TimeMS kWaitBudgetMs = 10000;

// How long the network thread stalls after a connection change. The user thread polls
// every millisecond, so it takes the Message inside the stall.
constexpr std::chrono::milliseconds kStall( 20 );

// The RawSystem never acknowledges anything, so the server's connection to it dies
// this long after it goes silent.
constexpr TimeMS kShortTimeoutMs = 300;

constexpr int kRounds = 8;

/// Stalls the network thread when it sends to either of two ports, at most once per
/// update cycle. The cycle's time is the same for every record it handles, so a
/// repeated time means the same cycle.
class StallOnSendPlugin : public PluginInterface2
{
public:
    bool UsesReliabilityLayer( void ) const override { return true; }

    void StallOnSendsTo( unsigned short first, unsigned short second )
    {
        m_firstPort = first;
        m_secondPort = second;
    }

    void OnInternalPacket( InternalPacket* internalPacket, unsigned frameNumber, SystemAddress remoteSystemAddress, RakNet::TimeMS time, int isSend ) override
    {
        (void)internalPacket;
        (void)frameNumber;
        if( isSend == false )
            return;

        const unsigned short port = remoteSystemAddress.GetPort();
        if( port != m_firstPort.load() && port != m_secondPort.load() )
            return;

        // Network thread only.
        if( time == m_lastStallTime )
            return;
        m_lastStallTime = time;
        std::this_thread::sleep_for( kStall );
    }

private:
    std::atomic<unsigned short> m_firstPort{ 0 };
    std::atomic<unsigned short> m_secondPort{ 0 };
    RakNet::TimeMS m_lastStallTime = 0;
};

/// What the user thread saw when it took each connection change.
struct Observations
{
    int checks = 0;
    int wrongAnswers = 0;
};

bool ViewShowsOpen( RakPeerInterface* peer, const Packet* packet )
{
    return peer->GetConnectionState( packet->systemAddress ) == IS_CONNECTED &&
           peer->GetGuidFromSystemAddress( packet->systemAddress ) == packet->guid &&
           peer->GetAveragePing( packet->systemAddress ) != -1;
}

bool ViewShowsClosed( RakPeerInterface* peer, const Packet* packet )
{
    return peer->GetConnectionState( packet->systemAddress ) == IS_NOT_CONNECTED;
}

/// Receive on the server, checking the view in every connection-change handler, and
/// drain the clients, until \a isAwaited holds for a Message. FAILs at the deadline.
void PollUntil( RakPeerInterface* server, RakPeerInterface* const* clients, Observations& observations, const std::function<bool( const Packet* )>& isAwaited )
{
    const TimeMS deadline = GetTimeMS() + kWaitBudgetMs;
    bool awaitedSeen = false;
    while( awaitedSeen == false )
    {
        for( Packet* packet = server->Receive(); packet != 0; packet = server->Receive() )
        {
            switch( packet->data[0] )
            {
            case ID_NEW_INCOMING_CONNECTION:
                observations.checks++;
                if( ViewShowsOpen( server, packet ) == false )
                    observations.wrongAnswers++;
                break;
            case ID_CONNECTION_LOST:
            case ID_DISCONNECTION_NOTIFICATION:
                observations.checks++;
                if( ViewShowsClosed( server, packet ) == false )
                    observations.wrongAnswers++;
                break;
            default:
                break;
            }
            if( isAwaited( packet ) )
                awaitedSeen = true;
            server->DeallocatePacket( packet );
        }
        ConnectionWaits::DrainAll( clients, 2 );

        if( ConnectionWaits::Expired( deadline ) )
            FAIL( "The awaited Message did not arrive" );
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }
}

} // namespace

TEST_CASE( "Receive hands out a connection change only once the published view shows it", "[network]" )
{
    WinsockFixture winsock;
    StallOnSendPlugin stall;
    PeerScope peers;

    RakPeerInterface* server = peers.Create();
    server->AttachPlugin( &stall );
    SocketDescriptor socketDescriptor( 0, "127.0.0.1" );
    REQUIRE( server->Startup( 3, &socketDescriptor, 1 ) == RAKNET_STARTED );
    server->SetMaximumIncomingConnections( 3 );
    const unsigned short serverPort = server->GetMyBoundAddress().GetPort();
    const SystemAddress serverAddress( "127.0.0.1", serverPort );

    RakPeerInterface* clients[2] = { peers.Client(), peers.Client() };
    const SystemAddress clientAddresses[2] = {
        SystemAddress( "127.0.0.1", clients[0]->GetMyBoundAddress().GetPort() ),
        SystemAddress( "127.0.0.1", clients[1]->GetMyBoundAddress().GetPort() ) };
    stall.StallOnSendsTo( clientAddresses[0].GetPort(), clientAddresses[1].GetPort() );

    // Keeps the server sending to both clients, so every update cycle stalls once they
    // are connected.
    std::atomic<bool> pumping{ true };
    std::thread pump( [&] {
        const unsigned char payload = ID_USER_PACKET_ENUM;
        while( pumping.load() )
        {
            for( const SystemAddress& address : clientAddresses )
                server->Send( (const char*)&payload, 1, HIGH_PRIORITY, UNRELIABLE, 0, address, false );
            std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
        }
    } );
    struct JoinPump
    {
        std::atomic<bool>& pumping;
        std::thread& pump;
        ~JoinPump()
        {
            pumping = false;
            pump.join();
        }
    } joinPump{ pumping, pump };

    Observations observations;
    for( int round = 0; round < kRounds; round++ )
    {
        INFO( "round " << round );
        const RakNetGUID rawGuid( 0x5741000000000000ull + (uint64_t)round );
        RawSystem raw( serverAddress, rawGuid.g );

        // The server holds the RawSystem's record from here, so the clients' records
        // come after it.
        raw.CompleteOfflineHandshake();
        for( RakPeerInterface* client : clients )
            REQUIRE( client->Connect( "127.0.0.1", serverPort, 0, 0 ) == CONNECTION_ATTEMPT_STARTED );
        int clientsConnected = 0;
        PollUntil( server, clients, observations, [&]( const Packet* packet ) {
            if( packet->data[0] == ID_NEW_INCOMING_CONNECTION && packet->guid != rawGuid )
                clientsConnected++;
            return clientsConnected == 2;
        } );

        raw.CompleteConnectionRequest();
        SystemAddress rawAddress;
        PollUntil( server, clients, observations, [&]( const Packet* packet ) {
            if( packet->data[0] != ID_NEW_INCOMING_CONNECTION || packet->guid != rawGuid )
                return false;
            rawAddress = packet->systemAddress;
            return true;
        } );

        // The network thread closes the connection either way: on a timeout, or once it
        // has acknowledged the RawSystem's notification.
        MessageID closedWith;
        if( round % 2 == 0 )
        {
            closedWith = ID_CONNECTION_LOST;
            server->SetTimeoutTime( kShortTimeoutMs, rawAddress );
        }
        else
        {
            closedWith = ID_DISCONNECTION_NOTIFICATION;
            BitStream notification;
            notification.Write( (MessageID)ID_DISCONNECTION_NOTIFICATION );
            raw.SendUnreliable( notification );
        }
        PollUntil( server, clients, observations, [&]( const Packet* packet ) {
            return packet->data[0] == closedWith && packet->guid == rawGuid;
        } );

        for( RakPeerInterface* client : clients )
            client->CloseConnection( serverAddress, true );
        int clientsClosed = 0;
        PollUntil( server, clients, observations, [&]( const Packet* packet ) {
            if( packet->data[0] == ID_DISCONNECTION_NOTIFICATION && packet->guid != rawGuid )
                clientsClosed++;
            return clientsClosed == 2;
        } );
        for( RakPeerInterface* client : clients )
            ConnectionWaits::WaitForDisconnect( client, serverAddress );
    }

    INFO( observations.wrongAnswers << " of " << observations.checks << " connection changes reached Receive before the view showed them" );
    CHECK( observations.wrongAnswers == 0 );
}
