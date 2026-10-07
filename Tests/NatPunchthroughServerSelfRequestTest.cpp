#include "Plugins/NatPunchthroughServer.h"

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
#include <thread>
#include <vector>

/*
NatPunchthroughServer refuses a punchthrough request a System makes to its own RakNetGUID,
and ignores a request from a System it has no User for.

The self-request regression: OnNATPunchthroughRequest compared the attempt's sender with its
recipient before it had set the recipient, so the check never fired. The attempt was pushed
onto the sender's list and the recipient's - the same list twice - and punchthrough started
against the sender itself. When the sender disconnected, OnClosedConnection erased from the
list it was range-for'ing over. Under MSVC debug iterators that aborts; elsewhere it is
undefined behaviour.

The missing-User regression: a System that connected before the plugin was attached has no
User, and the request indexed users with whatever GetIndexFromKey returned for the miss,
behind only a RakAssert.

The server answers the refusal with ID_NAT_TARGET_NOT_CONNECTED. Accepting it instead answers
with ID_NAT_GET_MOST_RECENT_PORT, so the first message the client gets back tells the two
apart.
*/

using namespace RakNet;

namespace {

constexpr unsigned short kServerPort = 30000;

// Hang guard for the server's answer. On loopback it arrives a few update cycles after the
// request, tens of milliseconds.
constexpr TimeMS kAnswerBudgetMs = 5000;

// How long to wait for an answer that should never come. The request is reliable, so the
// server has it and has answered, if it will, well inside this.
constexpr TimeMS kSilenceMs = 500;

void SendPunchthroughRequest( RakPeerInterface* client, RakNetGUID target, const SystemAddress& server )
{
    BitStream request;
    request.Write( (MessageID)ID_NAT_PUNCHTHROUGH_REQUEST );
    request.Write( target );
    client->Send( &request, HIGH_PRIORITY, RELIABLE_ORDERED, 0, server, false );
}

// Polls both peers until the client receives a NAT message or the deadline passes, and
// returns the message ids of every NAT message the client received.
std::vector<MessageID> CollectNatAnswers( RakPeerInterface* server, RakPeerInterface* client, TimeMS deadline, bool stopAtFirst )
{
    std::vector<MessageID> answers;
    while( !ConnectionWaits::Expired( deadline ) && !( stopAtFirst && !answers.empty() ) )
    {
        ConnectionWaits::Drain( server );
        for( Packet* packet = client->Receive(); packet != nullptr; packet = client->Receive() )
        {
            if( packet->data[0] >= ID_NAT_PUNCHTHROUGH_REQUEST && packet->data[0] <= ID_NAT_PUNCHTHROUGH_SUCCEEDED )
                answers.push_back( packet->data[0] );
            client->DeallocatePacket( packet );
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( ConnectionWaits::kPollInterval ) );
    }
    return answers;
}

// Drains the server until it hands out ID_DISCONNECTION_NOTIFICATION, which it does after
// every plugin's OnClosedConnection has run. Returns whether it did before the deadline.
bool WaitForDisconnectionNotification( RakPeerInterface* server )
{
    const TimeMS deadline = GetTimeMS() + kAnswerBudgetMs;
    while( !ConnectionWaits::Expired( deadline ) )
    {
        for( Packet* packet = server->Receive(); packet != nullptr; packet = server->Receive() )
        {
            const bool disconnected = packet->data[0] == ID_DISCONNECTION_NOTIFICATION;
            server->DeallocatePacket( packet );
            if( disconnected )
                return true;
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( ConnectionWaits::kPollInterval ) );
    }
    return false;
}

} // namespace

TEST_CASE( "NatPunchthroughServer refuses a punchthrough request to the sender's own guid", "[natpunchthrough][network]" )
{
    // Before the PeerScope, so it outlives the peers.
    NatPunchthroughServer punchServer;

    PeerScope peers;
    RakPeerInterface* server = peers.Server( kServerPort );
    RakPeerInterface* client = peers.Client();
    server->AttachPlugin( &punchServer );

    ConnectionWaits::ConnectAndWait( client, server );

    const SystemAddress serverAddress( "127.0.0.1", kServerPort );
    SendPunchthroughRequest( client, client->GetMyGUID(), serverAddress );
    const std::vector<MessageID> answers = CollectNatAnswers( server, client, GetTimeMS() + kAnswerBudgetMs, true );

    // Disconnect before checking, so OnClosedConnection walks the sender's attempts whatever
    // the answer was. The plugin sees the disconnect from Receive, so the server is drained
    // until the notification comes out of it.
    client->CloseConnection( serverAddress, true );
    REQUIRE( WaitForDisconnectionNotification( server ) );

    REQUIRE( !answers.empty() );
    CHECK( answers[0] == ID_NAT_TARGET_NOT_CONNECTED );

    server->DetachPlugin( &punchServer );
}

TEST_CASE( "NatPunchthroughServer ignores a punchthrough request from a system it has no user for", "[natpunchthrough][network]" )
{
    NatPunchthroughServer punchServer;

    PeerScope peers;
    RakPeerInterface* server = peers.Server( kServerPort );
    RakPeerInterface* client = peers.Client();

    ConnectionWaits::ConnectAndWait( client, server );

    // Attached after the connection, and after ConnectAndWait's drain has handed out its
    // ID_NEW_INCOMING_CONNECTION, so OnNewConnection never runs and the client has no User.
    server->AttachPlugin( &punchServer );

    const SystemAddress serverAddress( "127.0.0.1", kServerPort );
    SendPunchthroughRequest( client, RakNetGUID( 1001 ), serverAddress );
    const std::vector<MessageID> answers = CollectNatAnswers( server, client, GetTimeMS() + kSilenceMs, false );

    CHECK( answers.empty() );

    server->DetachPlugin( &punchServer );
}

TEST_CASE( "NatPunchthroughServer::User::TakeConnectionAttempts returns an attempt listed twice once", "[natpunchthrough]" )
{
    // The shape the old self-request left behind: one attempt on both sides of one User.
    NatPunchthroughServer::User user;
    NatPunchthroughServer::User other;
    NatPunchthroughServer::ConnectionAttempt self;
    self.sender = &user;
    self.recipient = &user;
    NatPunchthroughServer::ConnectionAttempt outgoing;
    outgoing.sender = &user;
    outgoing.recipient = &other;
    user.connectionAttempts = { &self, &outgoing, &self };

    const std::vector<NatPunchthroughServer::ConnectionAttempt*> taken = user.TakeConnectionAttempts();

    CHECK( taken == std::vector<NatPunchthroughServer::ConnectionAttempt*>{ &self, &outgoing } );
    CHECK( user.connectionAttempts.empty() );
}
