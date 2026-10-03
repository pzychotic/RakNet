#include "ConnectionWaits.h"
#include "PeerScope.h"
#include "RawSystem.h"

#include "BitStream.h"
#include "GetTime.h"
#include "MessageIdentifiers.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

/*
Pins what a Peer does with an ID_CONNECTION_REQUEST that is too short to read, arriving
as the first message on a connection that has just finished the offline handshake.

The UNVERIFIED_SENDER arm of RunUpdateCycle dispatches on data[0] alone. Every other arm
in that loop carries a byteSize test and this one does not, so a one-byte
ID_CONNECTION_REQUEST used to reach a reader that made three unchecked reads into three
uninitialised locals. BitStream::ReadBits leaves its output untouched when the stream is
short, so the guid, the timestamp and the doSecurity flag all held whatever was on the
stack - and the reader carried on regardless, replying with an
ID_CONNECTION_REQUEST_ACCEPTED whose echoed timestamp was that stack content. The
requester reads that value back as its own send-ping time; ConnectionRequestEchoTest has
the rest of that story, from the other reader of the same layout.

Unlike ticket 05's branch this needs no race. Any System that can complete the offline
handshake reaches it, one message later.

The check is on the reply rather than on the values, deliberately. What an uninitialised
local holds is not a property a test can assert - a sentinel of 0 would have passed
against the bug by coincidence - but whether the Peer answers at all is exact. Before the
fix a truncated request drew an ID_CONNECTION_REQUEST_ACCEPTED every time: the password
comparison it had to clear first passes trivially on a Peer with no password, because a
failed Read leaves the read offset where it was and the remaining-bytes count comes out
at zero either way. After the fix it draws no reply and the sender is banned.

Reaching UNVERIFIED_SENDER means being a System rather than driving one: a real Peer sends
its own well-formed ID_CONNECTION_REQUEST the instant the handshake completes, and there
is no supported way to ask it for a malformed one. So the far side of this test is a plain
UDP socket that speaks the handshake itself and then hand-builds one reliability datagram,
the way SplitPacketReassemblyTest hand-builds its own for the same reason.

RakPeerInterface functions explicitly tested:

    IsBanned

Exercised indirectly by getting to that point: Startup, SetMaximumIncomingConnections.
*/

using namespace RakNet;

namespace {

using namespace RawSystemHarness;

constexpr unsigned short kServerPort = 30000;

// Hang guard on the ban, not a tuning knob: over loopback the ban lands within an update
// cycle of the request arriving, so expiry means the Peer never decided.
constexpr TimeMS kBanBudgetMs = 5000;

// How long to listen for a reply once the ban shows the request was handled. The ban and
// any reply come out of the same arm of the same update cycle. SendImmediate queues a reply
// in that cycle and the next one sends it, so this is many update cycles of slack past the
// decision.
constexpr TimeMS kReplyWindowMs = 200;

// Any value, as long as no System already holds it - the server answers a duplicate guid
// with ID_ALREADY_CONNECTED rather than opening a connection slot. Nothing else here
// reads it.
constexpr uint64_t kRawSystemGuid = 0x00ABCDEF12345678ull;

} // namespace

TEST_CASE( "A truncated connection request from an unverified sender draws no reply", "[network]" )
{
    WinsockFixture winsock;
    PeerScope peers;

    RakPeerInterface* server = peers.Server( kServerPort );

    const SystemAddress serverAddress( "127.0.0.1", kServerPort );
    RawSystem rawSystem( serverAddress, kRawSystemGuid );
    rawSystem.CompleteOfflineHandshake();

    // One byte: the message id and nothing else. The writer emits 18 bytes at minimum -
    // MessageID | RakNetGUID | RakNet::Time | doSecurity - so all three reads come up
    // short, and none of the three values the reader goes on to use was ever written.
    BitStream truncatedRequest;
    truncatedRequest.Write( (MessageID)ID_CONNECTION_REQUEST );
    rawSystem.SendUnreliable( truncatedRequest );

    // The ban is how the Peer answers this request, so it marks the update cycle that
    // handled it; the reply check below listens from there.
    const TimeMS banDeadline = GetTimeMS() + kBanBudgetMs;
    while( !server->IsBanned( "127.0.0.1" ) && !ConnectionWaits::Expired( banDeadline ) )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( ConnectionWaits::kPollInterval ) );
    }

    // The whole defect in one line: before the fix an ID_CONNECTION_REQUEST_ACCEPTED came
    // back, carrying an echoed timestamp read from nothing. Stated as "not that message"
    // rather than "no datagram at all" so that a Peer which one day answers a stranger
    // with something honest - an error, a disconnection - would not fail it.
    char reply[MAXIMUM_MTU_SIZE];
    int replyLength = 0;
    CHECK_FALSE( rawSystem.WaitForMessage( ID_CONNECTION_REQUEST_ACCEPTED, RawSystem::Framing::Connected, static_cast<int>( kReplyWindowMs ), reply, replyLength ) );

    // And the sender is turned away rather than merely unanswered. UNVERIFIED_SENDER is the
    // state in which a Peer decides whether a stranger is talking sense, and a first message
    // it cannot read is handled the way that arm handles every other one: close, and ban the
    // address for the connection's timeout. This is the assertion that separates that choice
    // from the other one the ticket offered, dropping the message in silence, which would
    // leave it false.
    //
    // The ban is the whole of it. "The connection is gone" cannot be said through
    // NumberOfConnections, which counts only Systems in CONNECTED: a truncated request never
    // got one there, so that count is 0 against the fixed and the unfixed reader alike.
    CHECK( server->IsBanned( "127.0.0.1" ) );
}
