#include "PeerScope.h"

#include "BitStream.h"
#include "CommonFunctions.h"
#include "MessageIdentifiers.h"
#include "RakNetStringMakers.h"
#include "RakNetTypes.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>

/*
Pins the length of the ID_INVALID_PASSWORD a server sends when a client offers
the wrong password.

RakPeer::ParseConnectionRequestPacket built the message correctly and then handed
SendImmediate a BYTE count where its parameter is `BitSize_t numberOfBitsToSend`,
so the nine-byte message went out as nine BITS. The MessageID survived - it is the
first eight of those nine - which is why the defect is invisible to any test that
only waits for the ID to arrive. What did not survive is everything after it: the
sender's RakNetGUID lost 63 of its 64 bits, and the packet handed to the
application was two bytes rather than nine.

So the assertion here is on the LENGTH and the PAYLOAD, never on the arrival. A
test that stopped at "ID_INVALID_PASSWORD was received" passes against the unfixed
code.

The correct-password case is the control. Without it a run where the client never
reached the password comparison at all - a bind failure, a handshake change, an
incoming-connection limit - would look identical to a run where it did: the
rejection test would fail on a null packet either way, and the reason would not be
in the output.

RakPeerInterface functions explicitly tested:

    SetIncomingPassword
    Connect (with passwordData)

Exercised indirectly by getting to that point: Startup,
SetMaximumIncomingConnections, GetMyGUID, Receive, DeallocatePacket.
*/

using namespace RakNet;

namespace {

constexpr unsigned short kServerPort = 60000;

// One round trip over loopback: the client's ID_CONNECTION_REQUEST out, the
// server's rejection back. Generous by two orders of magnitude, and a hang guard
// rather than a tuning knob - expiry means the rejection was never sent, never
// that the machine was busy.
constexpr int kRejectBudgetMs = 5000;

constexpr int kConnectBudgetMs = 5000;

const char kServerPassword[] = "correct horse";

// Deliberately a different LENGTH as well as different content. RakPeer compares
// the length first (RakPeer.cpp), so a wrong password of equal length and a wrong
// password of unequal length take the same branch by different routes; the
// unequal one is the cheaper of the two to be sure about.
const char kWrongPassword[] = "battery";

// MessageID | RakNetGUID, field for field as ParseConnectionRequestPacket writes
// it, and through RakNetGUID::g because that is the ONLY member BitStream::Write
// serialises - sizeof( RakNetGUID ) would count systemIndex, which never goes on
// the wire. Spelled out rather than hardcoded as 9 so that a change to its
// serialised form shows up here as a diff rather than as a wrong magic number.
constexpr size_t kExpectedLength = sizeof( MessageID ) + sizeof( RakNetGUID::g );

} // namespace

TEST_CASE( "A rejected password produces an ID_INVALID_PASSWORD carrying the server's whole GUID", "[network]" )
{
    PeerScope peers;

    RakPeerInterface* server = peers.Server( kServerPort );
    server->SetIncomingPassword( kServerPassword, (int)strlen( kServerPassword ) );

    RakPeerInterface* client = peers.Client();

    REQUIRE( client->Connect( "127.0.0.1", kServerPort, kWrongPassword, (int)strlen( kWrongPassword ) ) == CONNECTION_ATTEMPT_STARTED );

    Packet* rejection = CommonFunctions::WaitAndReturnMessageWithID( client, ID_INVALID_PASSWORD, kRejectBudgetMs );

    // REQUIRE: everything below reads through it, and its absence is its own
    // diagnosis - the server never reached the password comparison.
    REQUIRE( rejection != nullptr );

    const unsigned int length = rejection->length;

    BitStream payload( rejection->data, rejection->length, false );
    payload.IgnoreBytes( sizeof( MessageID ) );
    RakNetGUID reportedGuid = UNASSIGNED_RAKNET_GUID;
    const bool guidRead = payload.Read( reportedGuid );

    client->DeallocatePacket( rejection );

    // The defect, stated directly. Nine bytes were built and nine BITS were sent,
    // so BITS_TO_BYTES rounded the arrival up to two.
    INFO( "received " << length << " bytes, expected " << kExpectedLength );
    CHECK( length == kExpectedLength );

    // Not implied by the length above: a message of the right length could still
    // carry the wrong eight bytes. This is the field the truncation destroyed, and
    // the server's own GUID is the only value it is ever allowed to hold.
    REQUIRE( guidRead );
    CHECK( reportedGuid == server->GetMyGUID() );
}

TEST_CASE( "The correct password connects, so the rejection above is a rejection rather than a failure to arrive", "[network]" )
{
    PeerScope peers;

    RakPeerInterface* server = peers.Server( kServerPort );
    server->SetIncomingPassword( kServerPassword, (int)strlen( kServerPassword ) );

    RakPeerInterface* client = peers.Client();

    REQUIRE( client->Connect( "127.0.0.1", kServerPort, kServerPassword, (int)strlen( kServerPassword ) ) == CONNECTION_ATTEMPT_STARTED );

    // Waits for the acceptance itself rather than for the connection state to
    // settle: IS_NOT_CONNECTED satisfies a settle wait, so a settle wait here
    // would pass on the very outcome this case exists to rule out.
    Packet* accepted = CommonFunctions::WaitAndReturnMessageWithID( client, ID_CONNECTION_REQUEST_ACCEPTED, kConnectBudgetMs );
    REQUIRE( accepted != nullptr );
    client->DeallocatePacket( accepted );
}
