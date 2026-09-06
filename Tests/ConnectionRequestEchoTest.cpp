#include "PeerScope.h"

#include "BitStream.h"
#include "CommonFunctions.h"
#include "ConnectionWaits.h"
#include "GetTime.h"
#include "MessageIdentifiers.h"
#include "RakNetDefines.h"
#include "RakNetTime.h"
#include "RakNetTypes.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

/*
Pins the timestamp a System echoes when an ID_CONNECTION_REQUEST arrives on a
connection it already considers established.

RakPeer has two readers for ID_CONNECTION_REQUEST. ParseConnectionRequestPacket
handles the normal case; the "already connected, reply anyway" branch of
RunUpdateCycle handles the simultaneous-connect race, where both Peers issued a
Connect at once and neither is in REQUESTED_CONNECTION any more. That second
reader used to skip a 16-byte OFFLINE_MESSAGE_DATA_ID that this message has never
carried, putting its timestamp read past the end of an 18-byte message. The read
failed, BitStream::ReadBits left the uninitialised local untouched, and whatever
was on the stack went out in ID_CONNECTION_REQUEST_ACCEPTED - where the requester
takes it for its own send-ping time and hands it to OnConnectedPong.

Reaching that branch through an actual simultaneous connect would be a race with
nothing to synchronise on, so the request is injected instead: a Peer that is
already connected sends the exact bytes ProcessOfflineNetworkPacket writes, and
the far side takes the same branch the race would have taken. The reply is read
off the wire rather than inferred from the ping table, for two reasons that both
make a ping assertion here meaningless:

  - a genuine loopback ping is legitimately 0 ms, so "GetLowestPing is not 0"
    does not separate a healthy connection from a poisoned one; and
  - the accepted-message handler calls PingInternal on the spot when it was not
    already connected (RakPeer.cpp), so a real ping/pong lands in the table
    milliseconds later and overwrites whatever the injected exchange put there.

The echoed value is the whole defect, and it is exactly comparable. The
timestamp sent is deliberately backdated: the value that used to come back was
indeterminate, and a value equal to "now" would be hard to distinguish from a
timestamp the far side substituted for one it never read.

RakPeerInterface functions explicitly tested:

    AllowConnectionResponseIPMigration
    Send (of a hand-built ID_CONNECTION_REQUEST)

Exercised indirectly by getting to that point: Startup,
SetMaximumIncomingConnections, Connect, Receive, DeallocatePacket,
GetGuidFromSystemAddress.
*/

using namespace RakNet;

namespace {

constexpr unsigned short kServerPort = 60000;

constexpr int kConnectBudgetMs = 5000;

// The injected request travels one hop and the reply one hop back, both
// RELIABLE_ORDERED over loopback. Generous by two orders of magnitude, and a
// hang guard rather than a tuning knob: expiry means the branch was never taken,
// never that the machine was busy.
constexpr int kReplyBudgetMs = 5000;

// How far in the past the injected timestamp is placed. Any value works - the
// far side only copies it - so this is chosen to be recognisable rather than
// realistic: far enough from "now" that a substituted timestamp could not be
// mistaken for it, small enough to stay a plausible ping.
constexpr Time kBackdatedMs = 250;

// RakNet's clock is process-relative and starts at zero (GetTimeTest), so on a
// fast machine the whole connect can finish before it reaches kBackdatedMs, and
// subtracting then would wrap to a timestamp far in the future - the exact shape
// of corruption this test exists to detect, manufactured by the test itself.
//
// Waiting rather than clamping to zero, which was the first attempt and was
// worse: zero is the likeliest value an uninitialised stack slot holds, so a
// clamped run passed against the unfixed reader by coincidence. The wait is a
// quarter second at its longest and usually nothing, because the connect above
// has already spent most of it.
void WaitForBackdatingToBePossible()
{
    while( RakNet::GetTime() <= kBackdatedMs )
        std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
}

// MessageID | RakNetGUID | RakNet::Time | doSecurity, field for field as
// ProcessOfflineNetworkPacket writes it (RakPeer.cpp). Kept in that order so a
// wire-format change shows up here as a diff. The trailing password bytes the
// writer appends are omitted: this Peer connected without one.
void WriteConnectionRequest( BitStream& out, RakNetGUID senderGuid, Time timestamp )
{
    out.Write( (MessageID)ID_CONNECTION_REQUEST );
    out.Write( senderGuid );
    out.Write( timestamp );
    out.Write( (unsigned char)0 ); // doSecurity
}

// The send-ping time out of ID_CONNECTION_REQUEST_ACCEPTED, read with the same
// sequence RakPeer::OnConnectionRequest writes and RunUpdateCycle reads back.
// Offsets are never computed by hand here: SystemAddress has a serialised form
// of its own, and reading through it is what keeps this in step with it.
bool ReadEchoedPingTime( const Packet* packet, Time& echoedPingTime )
{
    BitStream reply( packet->data, packet->length, false );
    reply.IgnoreBits( 8 );

    SystemAddress externalId;
    SystemIndex systemIndex;
    if( !reply.Read( externalId ) || !reply.Read( systemIndex ) )
        return false;

    for( unsigned int i = 0; i < MAXIMUM_NUMBER_OF_INTERNAL_IDS; i++ )
    {
        SystemAddress theirInternalId;
        if( !reply.Read( theirInternalId ) )
            return false;
    }

    return reply.Read( echoedPingTime );
}

} // namespace

TEST_CASE( "A connection request arriving on an established connection is echoed back with the timestamp it carried", "[network]" )
{
    PeerScope peers;

    RakPeerInterface* client = peers.Client();

    // Unnamed: everything below drives the client, and the server is only ever
    // spoken to. PeerScope keeps it alive and shuts it down.
    peers.Server( kServerPort );

    // Without this the client discards the second ID_CONNECTION_REQUEST_ACCEPTED
    // unread: that handler only accepts one while the connection is still being
    // established, and by then this one is CONNECTED. It changes what the client
    // does with the reply, not what the server puts in it.
    client->AllowConnectionResponseIPMigration( true );

    REQUIRE( client->Connect( "127.0.0.1", kServerPort, 0, 0 ) == CONNECTION_ATTEMPT_STARTED );
    REQUIRE( CommonFunctions::WaitAndConnect( client, "127.0.0.1", kServerPort, kConnectBudgetMs ) );

    const SystemAddress serverAddress( "127.0.0.1", kServerPort );
    const RakNetGUID serverGuid = client->GetGuidFromSystemAddress( serverAddress );
    REQUIRE( serverGuid != UNASSIGNED_RAKNET_GUID );

    // The handshake's own ID_CONNECTION_REQUEST_ACCEPTED is still queued. Drained
    // here so the one collected below is unambiguously the reply to the injected
    // request rather than the one the connect produced.
    ConnectionWaits::Drain( client );

    WaitForBackdatingToBePossible();
    const Time sentTimestamp = RakNet::GetTime() - kBackdatedMs;

    BitStream request;
    // GetMyGUID, where the writer in Source/ spells the same thing
    // GetGuidFromSystemAddress( UNASSIGNED_SYSTEM_ADDRESS ). Same value, and the
    // sentinel-address walk is an idiom a test should not depend on.
    WriteConnectionRequest( request, client->GetMyGUID(), sentTimestamp );

    // Addressed by GUID, not by address: Send treats one of our own addresses as a
    // loopback send and would push this onto the client's own receive queue.
    REQUIRE( client->Send( &request, IMMEDIATE_PRIORITY, RELIABLE_ORDERED, 0, serverGuid, false ) != 0 );

    Packet* reply = CommonFunctions::WaitAndReturnMessageWithID( client, ID_CONNECTION_REQUEST_ACCEPTED, kReplyBudgetMs );

    // REQUIRE: everything below reads through it, and its absence is its own
    // diagnosis - the server took a different branch, or none.
    REQUIRE( reply != nullptr );

    Time echoedPingTime = 0;
    const bool read = ReadEchoedPingTime( reply, echoedPingTime );
    client->DeallocatePacket( reply );

    REQUIRE( read );

    INFO( "sent " << sentTimestamp << ", echoed " << echoedPingTime );
    CHECK( echoedPingTime == sentTimestamp );

    // Cannot fail on its own - the equality above implies it - and is here as the
    // diagnosis on a run where the equality has already failed, which is why it is
    // a CHECK rather than a REQUIRE above it. "The echoed value was in the future"
    // is the specific wrongness that matters: OnConnectedPong reads this back as a
    // send-ping time, and a send-ping time later than now yields a ping of 0, which
    // pins lowestPing there for the life of the connection. Stated on the value
    // because the ping table cannot carry it - see the header.
    CHECK( echoedPingTime <= RakNet::GetTime() );
}
