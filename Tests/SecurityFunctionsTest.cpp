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

#include "GetTime.h"
#include "RakNetTime.h"
#include "RakPeerInterface.h"
#include "RakNetTypes.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <thread>

/*
Puts one server behind an incoming password and a ban list, then walks a single
client through every gate: no password, the wrong password, the right password,
banned, unbanned by RemoveFromBanList, banned again, unbanned by ClearBanList.
Each gate is asserted in both directions - the three connections that must be
refused matter as much as the three that must succeed.

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

constexpr unsigned short kServerPort = 60000;

// Six call sites, three passwords, two budgets. Returns whether the connection
// happened rather than asserting, because half of the call sites expect it NOT to.
bool TryToConnect( RakPeerInterface* client, const SystemAddress& server, const char* password, int passwordLength, int millisecondsToWait )
{
    const TimeMS entryTime = GetTimeMS();

    while( !CommonFunctions::ConnectionStateMatchesOptions( client, server, true ) && GetTimeMS() - entryTime < static_cast<TimeMS>( millisecondsToWait ) )
    {
        // Only re-issue Connect when nothing is already in flight, or the
        // second attempt is refused as a duplicate.
        if( !CommonFunctions::ConnectionStateMatchesOptions( client, server, true, true, true, true ) )
        {
            client->Connect( "127.0.0.1", server.GetPort(), password, passwordLength );
        }

        std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
    }

    return CommonFunctions::ConnectionStateMatchesOptions( client, server, true );
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
    REQUIRE_FALSE( TryToConnect( client, serverAddress, 0, 0, 5000 ) );

    const std::string badPassword = "badpass";
    REQUIRE_FALSE( TryToConnect( client, serverAddress, badPassword.c_str(), static_cast<int>( badPassword.size() ), 5000 ) );

    // 50 s rather than the 5 s every other attempt gets. Why the first accepted
    // connection is the slow one has never been explained; the budget is a ceiling
    // a healthy run does not approach, not a measurement.
    REQUIRE( TryToConnect( client, serverAddress, thePassword.c_str(), static_cast<int>( thePassword.size() ), 50000 ) );

    Disconnect( client, serverAddress, "the correct password was accepted" );

    // Each ban section starts from a disconnected client and ends by disconnecting
    // again, so a failure in one does not poison the next - hence CHECK from here
    // down, and a broken ban list reports every gate it breaks in a single run.
    server->AddToBanList( "127.0.0.1", 0 );
    CHECK( server->IsBanned( "127.0.0.1" ) );
    CHECK_FALSE( TryToConnect( client, serverAddress, thePassword.c_str(), static_cast<int>( thePassword.size() ), 5000 ) );

    server->RemoveFromBanList( "127.0.0.1" );
    CHECK_FALSE( server->IsBanned( "127.0.0.1" ) );
    CHECK( TryToConnect( client, serverAddress, thePassword.c_str(), static_cast<int>( thePassword.size() ), 5000 ) );

    Disconnect( client, serverAddress, "RemoveFromBanList let the client back in" );

    // The same ban, lifted the other way, which is the only reason this second
    // half exists.
    server->AddToBanList( "127.0.0.1", 0 );
    CHECK( server->IsBanned( "127.0.0.1" ) );
    CHECK_FALSE( TryToConnect( client, serverAddress, thePassword.c_str(), static_cast<int>( thePassword.size() ), 5000 ) );

    server->ClearBanList();
    CHECK_FALSE( server->IsBanned( "127.0.0.1" ) );
    CHECK( TryToConnect( client, serverAddress, thePassword.c_str(), static_cast<int>( thePassword.size() ), 5000 ) );
}
