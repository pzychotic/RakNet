/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "CommonFunctions.h"
#include "ConnectionWaits.h"
#include "PeerScope.h"
#include "RawSystem.h"

#include "GetTime.h"
#include "MessageIdentifiers.h"
#include "RakNetStatistics.h"
#include "RakNetStringMakers.h"
#include "RakNetTime.h"
#include "RakPeerInterface.h"
#include "RakNetTypes.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

/*
Puts one server behind an incoming password and a ban list, then walks a single
client through every gate: no password, the wrong password, the right password,
banned, unbanned by RemoveFromBanList, banned again, unbanned by ClearBanList.
Each gate is asserted in both directions - the three connections that must be
refused matter as much as the three that must succeed.

A refusal is asserted by the Message that reports it - ID_INVALID_PASSWORD for no
password or the wrong one, ID_CONNECTION_BANNED for a ban - and by the client not being
connected after it. So a refusal for the wrong reason fails, and so does a refusal that
stops sending its Message, even though the client is still kept out.

The three cases after it pin how a refusal ends. A refused attempt leaves a record on
both sides. The client acks the refusal before it drops its own record, which lets the
server drop its record too. If that ack never comes, the server replaces the record when
the same address tries again. Either way a retry is never told ID_ALREADY_CONNECTED.

RakPeerInterface functions explicitly tested:

    SetIncomingPassword
    GetIncomingPassword
    AddToBanList
    IsBanned
    RemoveFromBanList
    ClearBanList

Exercised indirectly by getting to that point: Startup,
SetMaximumIncomingConnections, Connect, CloseConnection, GetConnectionState.

NOT covered, and this is the record that the gap is known rather than lost:
InitializeSecurity, AddToSecurityExceptionList, IsInSecurityExceptionList and
RemoveFromSecurityExceptionList. Nothing here could assert anything about them in
this build - LIBCAT_SECURITY defaults to 0 (Source/NativeFeatureIncludes.h), which
compiles all four out to no-ops. Covering them needs that build option on and a
test written against the current three-argument InitializeSecurity( publicKey,
privateKey, bRequireClientKey ).
*/

using namespace RakNet;

namespace {

constexpr unsigned short kServerPort = 30000;

// For a refusal to arrive: one round trip over loopback. A hang guard rather than a
// tuning knob.
constexpr TimeMS kRefusalArrivalBudgetMs = 5000;

// For an accepted connection to come up. A hang guard too: an accepted attempt connects
// on its first try.
constexpr TimeMS kConnectBudgetMs = 5000;

// The client's state toward the server while an attempt is in flight or its record is
// still closing, so Connect is not re-issued into ALREADY_CONNECTED_TO_ENDPOINT.
bool AttemptInFlight( RakPeerInterface* client, const SystemAddress& server )
{
    return CommonFunctions::ConnectionStateMatchesOptions( client, server, true, true, true, true, false, false, true );
}

// Polls until connected, re-issuing Connect whenever nothing is in flight. Ends on the
// connection; the budget is a hang guard.
bool TryToConnect( RakPeerInterface* client, const SystemAddress& server, const char* password, int passwordLength )
{
    const TimeMS deadline = GetTimeMS() + kConnectBudgetMs;

    while( !CommonFunctions::ConnectionStateMatchesOptions( client, server, true ) && !ConnectionWaits::Expired( deadline ) )
    {
        if( !AttemptInFlight( client, server ) )
        {
            client->Connect( "127.0.0.1", server.GetPort(), password, passwordLength );
        }

        ConnectionWaits::Drain( client );
        std::this_thread::sleep_for( std::chrono::milliseconds( ConnectionWaits::kPollInterval ) );
    }

    return CommonFunctions::ConnectionStateMatchesOptions( client, server, true );
}

// Every Message that ends a connection attempt without connecting.
bool IsRefusal( MessageID id )
{
    switch( id )
    {
    case ID_CONNECTION_ATTEMPT_FAILED:
    case ID_ALREADY_CONNECTED:
    case ID_NO_FREE_INCOMING_CONNECTIONS:
    case ID_CONNECTION_BANNED:
    case ID_INVALID_PASSWORD:
    case ID_INCOMPATIBLE_PROTOCOL_VERSION:
    case ID_IP_RECENTLY_CONNECTED:
        return true;
    default:
        return false;
    }
}

// Connects once and receives until a refusal comes out. True when it is `refusal` and
// the client is not connected. Ends on the first refusal, so one for the wrong reason
// fails at once and names itself; the budgets are hang guards. Waits first for the
// client to let go of any record an earlier refusal left, which would turn this
// Connect into ALREADY_CONNECTED_TO_ENDPOINT.
bool RefusedWith( RakPeerInterface* client, const SystemAddress& server, const char* password, int passwordLength, MessageID refusal )
{
    const TimeMS recordDeadline = GetTimeMS() + kRefusalArrivalBudgetMs;
    while( AttemptInFlight( client, server ) && !ConnectionWaits::Expired( recordDeadline ) )
    {
        ConnectionWaits::Drain( client );
        std::this_thread::sleep_for( std::chrono::milliseconds( ConnectionWaits::kPollInterval ) );
    }

    const ConnectionAttemptResult attempt = client->Connect( "127.0.0.1", server.GetPort(), password, passwordLength );
    if( attempt != CONNECTION_ATTEMPT_STARTED )
    {
        UNSCOPED_INFO( "Connect returned " << static_cast<int>( attempt ) << ", state toward the server " << static_cast<int>( client->GetConnectionState( server ) ) );
        return false;
    }

    const TimeMS deadline = GetTimeMS() + kRefusalArrivalBudgetMs;
    int received = -1;
    while( received < 0 && !ConnectionWaits::Expired( deadline ) )
    {
        for( Packet* packet = client->Receive(); packet != nullptr; packet = client->Receive() )
        {
            if( received < 0 && IsRefusal( packet->data[0] ) )
                received = packet->data[0];
            client->DeallocatePacket( packet );
        }

        if( received < 0 )
            std::this_thread::sleep_for( std::chrono::milliseconds( ConnectionWaits::kPollInterval ) );
    }

    if( received < 0 )
    {
        UNSCOPED_INFO( "no refusal within " << kRefusalArrivalBudgetMs << " ms, expected message " << static_cast<int>( refusal ) );
        return false;
    }

    if( received != refusal )
    {
        UNSCOPED_INFO( "refused with message " << received << ", expected " << static_cast<int>( refusal ) );
        return false;
    }

    return !CommonFunctions::ConnectionStateMatchesOptions( client, server, true );
}

// Close once, then wait - never a poll that re-issues the close, which livelocks.
// See ConnectionWaits::WaitForDisconnect.
//
// Both call sites disconnect the same client from the same server, so the wait's
// own message - which names the port and the state - cannot say which of them
// expired. Hence the label, which says which gate the client was being taken back
// out of.
void Disconnect( RakPeerInterface* client, const SystemAddress& server, const char* afterWhichGate )
{
    INFO( "disconnecting after " << afterWhichGate );

    client->CloseConnection( server, true, 0, LOW_PRIORITY );
    ConnectionWaits::WaitForDisconnect( client, server );
}

} // namespace

TEST_CASE( "SetIncomingPassword and the ban list decide which clients a server accepts", "[network]" )
{
    PeerScope peers;

    const std::string thePassword = "password";

    RakPeerInterface* server = peers.Server( kServerPort );
    server->SetIncomingPassword( thePassword.c_str(), static_cast<int>( thePassword.size() ) );

    RakPeerInterface* client = peers.Client();

    const SystemAddress serverAddress( "127.0.0.1", kServerPort );

    // A data block read back with its length, never null-terminated in place at
    // returnedPassword[returnedLength] - that writes one past the end whenever the
    // password fills the buffer.
    char returnedPassword[22];
    int returnedLength = sizeof( returnedPassword );
    server->GetIncomingPassword( returnedPassword, &returnedLength );

    // Nothing below reads this back: the gates that follow depend on
    // SetIncomingPassword having taken, not on GetIncomingPassword reporting it.
    CHECK( std::string( returnedPassword, returnedLength ) == thePassword );

    // REQUIRE, not CHECK, for the two refusals: they share one connection with the
    // acceptance below and there is no disconnect between them, so a client that
    // wrongly gets in here makes both following gates meaningless rather than
    // merely failed.
    REQUIRE( RefusedWith( client, serverAddress, 0, 0, ID_INVALID_PASSWORD ) );

    const std::string badPassword = "badpass";
    REQUIRE( RefusedWith( client, serverAddress, badPassword.c_str(), static_cast<int>( badPassword.size() ), ID_INVALID_PASSWORD ) );

    // Straight after two refusals, inside an accepted connection's own budget: neither
    // refusal leaves a record that blocks this attempt. The tests below pin why.
    REQUIRE( TryToConnect( client, serverAddress, thePassword.c_str(), static_cast<int>( thePassword.size() ) ) );

    Disconnect( client, serverAddress, "the correct password was accepted" );

    // Each ban section starts from a disconnected client and ends by disconnecting
    // again, so a failure in one does not poison the next - hence CHECK from here
    // down, and a broken ban list reports every gate it breaks in a single run.
    server->AddToBanList( "127.0.0.1", 0 );
    CHECK( server->IsBanned( "127.0.0.1" ) );
    CHECK( RefusedWith( client, serverAddress, thePassword.c_str(), static_cast<int>( thePassword.size() ), ID_CONNECTION_BANNED ) );

    server->RemoveFromBanList( "127.0.0.1" );
    CHECK_FALSE( server->IsBanned( "127.0.0.1" ) );
    CHECK( TryToConnect( client, serverAddress, thePassword.c_str(), static_cast<int>( thePassword.size() ) ) );

    Disconnect( client, serverAddress, "RemoveFromBanList let the client back in" );

    // The same ban, lifted the other way, which is the only reason this second
    // half exists.
    server->AddToBanList( "127.0.0.1", 0 );
    CHECK( server->IsBanned( "127.0.0.1" ) );
    CHECK( RefusedWith( client, serverAddress, thePassword.c_str(), static_cast<int>( thePassword.size() ), ID_CONNECTION_BANNED ) );

    server->ClearBanList();
    CHECK_FALSE( server->IsBanned( "127.0.0.1" ) );
    CHECK( TryToConnect( client, serverAddress, thePassword.c_str(), static_cast<int>( thePassword.size() ) ) );
}

namespace {

// Far under the server's timeout, 10 s in Release and 30 s in Debug. Expiry means the
// Refused System's record outlived its refusal and only the timeout let it go.
constexpr TimeMS kRefusalReleaseBudgetMs = 2000;

const char kRightPassword[] = "password";
const char kWrongPassword[] = "badpass";

// Connects with the wrong password and waits for the refusal, keeping the client's network
// thread cycling meanwhile: GetStatistics is answered on that thread and wakes it. Cycles
// that come faster than the reliability layer holds an ack back (one SYN, 10 ms) are what
// let a client drop its record before acking the refusal. Left to the 10 ms timer, a client
// mostly gets the ack out first.
bool RefuseWhileBusy( RakPeerInterface* client, const SystemAddress& server )
{
    if( client->Connect( "127.0.0.1", server.GetPort(), kWrongPassword, static_cast<int>( strlen( kWrongPassword ) ) ) != CONNECTION_ATTEMPT_STARTED )
        return false;

    const TimeMS deadline = GetTimeMS() + kRefusalArrivalBudgetMs;
    while( !ConnectionWaits::Expired( deadline ) )
    {
        RakNetStatistics statistics;
        client->GetStatistics( server, &statistics );

        for( Packet* packet = client->Receive(); packet != nullptr; packet = client->Receive() )
        {
            const bool refused = packet->data[0] == ID_INVALID_PASSWORD;
            client->DeallocatePacket( packet );
            if( refused )
                return true;
        }
    }

    return false;
}

// Connects with the right password, re-issuing Connect whenever the client holds nothing
// toward the server, until connected or the budget runs out. Drains as it goes and
// reports any ID_ALREADY_CONNECTED, which is the server refusing for the wrong reason.
bool ConnectWithin( RakPeerInterface* client, const SystemAddress& server, TimeMS budget, bool& sawAlreadyConnected )
{
    const TimeMS deadline = GetTimeMS() + budget;

    while( !CommonFunctions::ConnectionStateMatchesOptions( client, server, true ) && !ConnectionWaits::Expired( deadline ) )
    {
        if( !AttemptInFlight( client, server ) )
        {
            client->Connect( "127.0.0.1", server.GetPort(), kRightPassword, static_cast<int>( strlen( kRightPassword ) ) );
        }

        for( Packet* packet = client->Receive(); packet != nullptr; packet = client->Receive() )
        {
            if( packet->data[0] == ID_ALREADY_CONNECTED )
                sawAlreadyConnected = true;
            client->DeallocatePacket( packet );
        }

        std::this_thread::sleep_for( std::chrono::milliseconds( ConnectionWaits::kPollInterval ) );
    }

    return CommonFunctions::ConnectionStateMatchesOptions( client, server, true );
}

} // namespace

TEST_CASE( "A server lets go of a Refused System once the System acks the refusal", "[network]" )
{
    PeerScope peers;

    RakPeerInterface* server = peers.Server( kServerPort );
    server->SetIncomingPassword( kRightPassword, static_cast<int>( strlen( kRightPassword ) ) );

    RakPeerInterface* client = peers.Client();

    // The server sets its record closing in the call that sends the refusal, so from
    // here on the record exists and only the client's ack can let it go early.
    REQUIRE( RefuseWhileBusy( client, SystemAddress( "127.0.0.1", kServerPort ) ) );

    const RakNetGUID clientGuid = client->GetMyGUID();
    const TimeMS deadline = GetTimeMS() + kRefusalReleaseBudgetMs;
    while( server->GetConnectionState( clientGuid ) != IS_NOT_CONNECTED && !ConnectionWaits::Expired( deadline ) )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( ConnectionWaits::kPollInterval ) );
    }

    CHECK( server->GetConnectionState( clientGuid ) == IS_NOT_CONNECTED );
}

TEST_CASE( "A Peer restarted on a Refused System's port gets in although the refusal was never acked", "[network]" )
{
    using namespace RawSystemHarness;

    WinsockFixture winsock;
    PeerScope peers;

    RakPeerInterface* server = peers.Server( kServerPort );
    server->SetIncomingPassword( kRightPassword, static_cast<int>( strlen( kRightPassword ) ) );

    const SystemAddress serverAddress( "127.0.0.1", kServerPort );

    // A System that is refused and gone without a word, the way a Peer that is shut down
    // between the refusal and its ack would be. A real Peer acks within one update cycle,
    // too soon to be stopped reliably first.
    unsigned short refusedPort = 0;
    {
        RawSystem refused( serverAddress, 0x0E05ED );
        refused.CompleteOfflineHandshake();
        refused.SendConnectionRequest( kWrongPassword, static_cast<int>( strlen( kWrongPassword ) ) );

        char refusal[MAXIMUM_MTU_SIZE];
        int refusalLength = 0;
        REQUIRE( refused.WaitForMessage( ID_INVALID_PASSWORD, RawSystem::Framing::Connected, static_cast<int>( kRefusalArrivalBudgetMs ), refusal, refusalLength ) );

        refusedPort = refused.GetBoundPort();
    }

    RakPeerInterface* restarted = peers.Client( refusedPort );

    bool sawAlreadyConnected = false;
    CHECK( ConnectWithin( restarted, serverAddress, kRefusalReleaseBudgetMs, sawAlreadyConnected ) );
    CHECK_FALSE( sawAlreadyConnected );
}

TEST_CASE( "A client refused for a wrong password gets in with the right one", "[network]" )
{
    PeerScope peers;

    RakPeerInterface* server = peers.Server( kServerPort );
    server->SetIncomingPassword( kRightPassword, static_cast<int>( strlen( kRightPassword ) ) );

    RakPeerInterface* client = peers.Client();

    const SystemAddress serverAddress( "127.0.0.1", kServerPort );

    REQUIRE( RefuseWhileBusy( client, serverAddress ) );

    bool sawAlreadyConnected = false;
    CHECK( ConnectWithin( client, serverAddress, kRefusalReleaseBudgetMs, sawAlreadyConnected ) );
    CHECK_FALSE( sawAlreadyConnected );
}
