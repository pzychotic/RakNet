#include "PeerScope.h"

#include "BitStream.h"
#include "GetTime.h"
#include "MTUSize.h"
#include "MessageIdentifiers.h"
#include "RakNetSocket2.h"
#include "RakNetTypes.h"
#include "RakNetVersion.h"
#include "RakPeerInterface.h"
#include "ReliabilityLayer.h"
#include "SocketDefines.h"
#include "WSAStartupSingleton.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>

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

constexpr unsigned short kServerPort = 60000;

// Long enough that "no reply arrived" means the Peer chose not to send one rather than
// that it had not got round to it: the whole exchange is over loopback and the pre-fix
// reply landed within a single update cycle.
constexpr int kReplyBudgetMs = 2000;

// One handshake datagram out, one reply back, no retries. Generous by two orders of
// magnitude, and a hang guard rather than a tuning knob.
constexpr int kHandshakeBudgetMs = 5000;

// Any value, as long as no System already holds it - the server answers a duplicate guid
// with ID_ALREADY_CONNECTED rather than opening a connection slot. Nothing else here
// reads it.
constexpr uint64_t kRawSystemGuid = 0x00ABCDEF12345678ull;

// The 16-byte cookie every offline handshake message carries. Its only declaration in
// Source/ is a file-static in RakPeer.cpp, so a test that speaks the handshake has to
// spell it out; kept byte for byte against that one.
const unsigned char OFFLINE_MESSAGE_DATA_ID[16] = { 0x00, 0xFF, 0xFF, 0x00, 0xFE, 0xFE, 0xFE, 0xFE, 0xFD, 0xFD, 0xFD, 0xFD, 0x12, 0x34, 0x56, 0x78 };

// Nothing here goes through RakPeer::Startup, so this takes the same Winsock refcount
// RakNet itself does. A no-op off Windows.
struct WinsockFixture
{
    WinsockFixture() { WSAStartupSingleton::AddRef(); }
    ~WinsockFixture() { WSAStartupSingleton::Deref(); }
};

// ---------------------------------------------------------------------------
// Datagram construction
//
// Mirrors DatagramHeaderFormat::Serialize and
// ReliabilityLayer::WriteToBitStreamFromInternalPacket, both private to
// ReliabilityLayer.cpp, in the same field order so a wire-format change shows up here as
// a diff. UNRELIABLE throughout: it is the shortest of the encodings, and nothing below
// waits for an ack.
// ---------------------------------------------------------------------------

void WriteDatagramHeader( BitStream& out, DatagramSequenceNumberType datagramNumber )
{
    out.Write( true );  // isValid
    out.Write( false ); // isACK
    out.Write( false ); // isNAK
    out.Write( false ); // isPacketPair
    out.Write( false ); // isContinuousSend
    out.Write( false ); // needsBAndAs
    out.AlignWriteToByteBoundary();
#if INCLUDE_TIMESTAMP_WITH_DATAGRAMS == 1
    out.Write( (RakNet::TimeMS)0 );
#endif
    out.Write( datagramNumber );
}

void WriteUnreliableMessage( BitStream& out, const unsigned char* payload, unsigned int payloadLength )
{
    out.AlignWriteToByteBoundary();

    unsigned char reliability = (unsigned char)UNRELIABLE;
    out.WriteBits( &reliability, 3, true );
    out.Write( false ); // hasSplitPacket
    out.AlignWriteToByteBoundary();

    unsigned short dataBitLength = (unsigned short)BYTES_TO_BITS( payloadLength );
    out.WriteAlignedVar16( (const char*)&dataBitLength );
    out.AlignWriteToByteBoundary();

    out.WriteAlignedBytes( payload, payloadLength );
}

/// The id of the first message in a datagram, mirroring
/// ReliabilityLayer::CreateInternalPacketFromBitStream far enough to reach the payload.
/// False for an ACK or NAK datagram, which carries no message at all.
///
/// Needed because everything the far side sends once the connection record exists is
/// wrapped in this framing: reading the first byte off the wire, the way the handshake
/// above can, would only ever read a datagram header.
bool ReadFirstMessageId( const char* data, int length, MessageID& messageIdOut )
{
    BitStream in( (unsigned char*)data, (unsigned int)length, false );

    bool isValid = false, isACK = false, isNAK = false;
    bool isPacketPair = false, isContinuousSend = false, needsBAndAs = false;
    if( in.Read( isValid ) == false || in.Read( isACK ) == false )
        return false;
    if( isACK )
        return false;
    if( in.Read( isNAK ) == false || isNAK )
        return false;
    if( in.Read( isPacketPair ) == false || in.Read( isContinuousSend ) == false || in.Read( needsBAndAs ) == false )
        return false;
    in.AlignReadToByteBoundary();
#if INCLUDE_TIMESTAMP_WITH_DATAGRAMS == 1
    RakNet::TimeMS sourceSystemTime = 0;
    if( in.Read( sourceSystemTime ) == false )
        return false;
#endif
    DatagramSequenceNumberType datagramNumber;
    if( in.Read( datagramNumber ) == false )
        return false;

    in.AlignReadToByteBoundary();

    unsigned char reliability = 0;
    bool hasSplitPacket = false;
    if( in.ReadBits( &reliability, 3, true ) == false || in.Read( hasSplitPacket ) == false )
        return false;
    in.AlignReadToByteBoundary();

    unsigned short dataBitLength = 0;
    if( in.ReadAlignedVar16( (char*)&dataBitLength ) == false || dataBitLength == 0 )
        return false;

    if( reliability == RELIABLE || reliability == RELIABLE_SEQUENCED || reliability == RELIABLE_ORDERED )
    {
        MessageNumberType reliableMessageNumber;
        if( in.Read( reliableMessageNumber ) == false )
            return false;
    }
    in.AlignReadToByteBoundary();

    if( reliability == UNRELIABLE_SEQUENCED || reliability == RELIABLE_SEQUENCED )
    {
        OrderingIndexType sequencingIndex;
        if( in.Read( sequencingIndex ) == false )
            return false;
    }

    if( reliability == UNRELIABLE_SEQUENCED || reliability == RELIABLE_SEQUENCED || reliability == RELIABLE_ORDERED )
    {
        OrderingIndexType orderingIndex;
        unsigned char orderingChannel;
        if( in.Read( orderingIndex ) == false || in.ReadAlignedVar8( (char*)&orderingChannel ) == false )
            return false;
    }

    if( hasSplitPacket )
    {
        SplitPacketIndexType splitPacketCount, splitPacketIndex;
        SplitPacketIdType splitPacketId;
        if( in.ReadAlignedVar32( (char*)&splitPacketCount ) == false ||
            in.ReadAlignedVar16( (char*)&splitPacketId ) == false ||
            in.ReadAlignedVar32( (char*)&splitPacketIndex ) == false )
            return false;
    }

    return in.ReadAlignedBytes( &messageIdOut, sizeof( messageIdOut ) );
}

/// A bound UDP socket that speaks RakNet's offline handshake by hand, so the far side
/// reaches UNVERIFIED_SENDER with no Peer on this end to send a well-formed
/// ID_CONNECTION_REQUEST the moment it gets there.
class RawSystem
{
public:
    explicit RawSystem( const SystemAddress& serverAddress )
    : m_serverAddress( serverAddress )
    {
        char hostAddress[] = "127.0.0.1";

        RNS2_BerkleyBindParameters bindParameters;
        memset( &bindParameters, 0, sizeof( bindParameters ) );
        bindParameters.port = 0; // OS-assigned; the server reads it off the datagram
        bindParameters.hostAddress = hostAddress;
        bindParameters.addressFamily = AF_INET;
        bindParameters.type = SOCK_DGRAM;
        bindParameters.protocol = 0;
        bindParameters.nonBlockingSocket = false;
        bindParameters.eventHandler = 0; // No polling thread: every read here is explicit

        REQUIRE( m_socket.Bind( &bindParameters, _FILE_AND_LINE_ ) == BR_SUCCESS );
    }

    void Send( const BitStream& datagram )
    {
        RNS2_SendParameters sendParameters;
        sendParameters.data = (char*)datagram.GetData();
        sendParameters.length = (int)datagram.GetNumberOfBytesUsed();
        sendParameters.systemAddress = m_serverAddress;

        REQUIRE( m_socket.Send( &sendParameters, _FILE_AND_LINE_ ) == (RNS2SendResult)sendParameters.length );
    }

    /// Where a message's id is to be found. An Offline message is written straight to the
    /// socket, so it is the datagram's first byte; a Connected one has come through a
    /// reliability layer, so it has to be decoded out of the framing.
    enum class Framing
    {
        Offline,
        Connected
    };

    /// The next datagram to arrive, or false once millisecondsToWait is spent.
    bool WaitForDatagram( int millisecondsToWait, char* dataOut, int& lengthOut )
    {
        timeval timeout;
        timeout.tv_sec = (long)( millisecondsToWait / 1000 );
        timeout.tv_usec = (long)( ( millisecondsToWait % 1000 ) * 1000 );

        fd_set readable;
        FD_ZERO( &readable );
        FD_SET( m_socket.GetSocket(), &readable );

        if( select__( (int)m_socket.GetSocket() + 1, &readable, 0, 0, &timeout ) <= 0 )
            return false;

        sockaddr_storage from;
        socklen_t fromLength = sizeof( from );
        const int received = recvfrom__( m_socket.GetSocket(), dataOut, MAXIMUM_MTU_SIZE, 0, (sockaddr*)&from, &fromLength );
        if( received <= 0 )
            return false;

        lengthOut = received;
        return true;
    }

    /// The next datagram carrying this message id, or false once the budget is spent.
    /// Anything else is discarded and the wait continues: the server resends handshake
    /// replies, and none of them are what a caller here is waiting for.
    bool WaitForMessage( MessageID messageId, Framing framing, int millisecondsToWait, char* dataOut, int& lengthOut )
    {
        const RakNet::TimeMS deadline = RakNet::GetTimeMS() + (RakNet::TimeMS)millisecondsToWait;

        while( RakNet::GetTimeMS() < deadline )
        {
            const int remaining = (int)( deadline - RakNet::GetTimeMS() );
            if( WaitForDatagram( remaining, dataOut, lengthOut ) == false )
                return false;

            MessageID received = 0;
            if( framing == Framing::Offline )
                received = (unsigned char)dataOut[0];
            else if( ReadFirstMessageId( dataOut, lengthOut, received ) == false )
                continue;

            if( received == messageId )
                return true;
        }

        return false;
    }

    /// ID_OPEN_CONNECTION_REQUEST_1 and _2 and their replies, field for field as
    /// RakPeer::RunUpdateCycle and ProcessOfflineNetworkPacket write and read them, and in
    /// that order so a wire-format change shows up here as a diff.
    void CompleteOfflineHandshake()
    {
        BitStream request1;
        request1.Write( (MessageID)ID_OPEN_CONNECTION_REQUEST_1 );
        request1.WriteAlignedBytes( (const unsigned char*)OFFLINE_MESSAGE_DATA_ID, sizeof( OFFLINE_MESSAGE_DATA_ID ) );
        request1.Write( (MessageID)RAKNET_PROTOCOL_VERSION );
        // The padding is the MTU probe: the server sizes the connection from the length of
        // this datagram, so the largest of RakPeer's own mtuSizes is what a real first
        // attempt asks for.
        request1.PadWithZeroToByteLength( MAXIMUM_MTU_SIZE - UDP_HEADER_SIZE );
        Send( request1 );

        char reply1[MAXIMUM_MTU_SIZE];
        int reply1Length = 0;
        REQUIRE( WaitForMessage( ID_OPEN_CONNECTION_REPLY_1, Framing::Offline, kHandshakeBudgetMs, reply1, reply1Length ) );

        BitStream reply1Stream( (unsigned char*)reply1, (unsigned int)reply1Length, false );
        reply1Stream.IgnoreBytes( sizeof( MessageID ) );
        reply1Stream.IgnoreBytes( sizeof( OFFLINE_MESSAGE_DATA_ID ) );

        RakNetGUID serverGuid;
        unsigned char serverHasSecurity = 0;
        uint16_t mtu = 0;
        REQUIRE( reply1Stream.Read( serverGuid ) );
        REQUIRE( reply1Stream.Read( serverHasSecurity ) );

        // LIBCAT_SECURITY has never compiled in this fork, so the server never asks for a
        // cookie and there is no branch here for one.
        REQUIRE( serverHasSecurity == 0 );
        REQUIRE( reply1Stream.Read( mtu ) );

        BitStream request2;
        request2.Write( (MessageID)ID_OPEN_CONNECTION_REQUEST_2 );
        request2.WriteAlignedBytes( (const unsigned char*)OFFLINE_MESSAGE_DATA_ID, sizeof( OFFLINE_MESSAGE_DATA_ID ) );
        request2.Write( m_serverAddress ); // Binding address: the address being connected to
        request2.Write( mtu );
        request2.Write( RakNetGUID( kRawSystemGuid ) );
        Send( request2 );

        char reply2[MAXIMUM_MTU_SIZE];
        int reply2Length = 0;
        REQUIRE( WaitForMessage( ID_OPEN_CONNECTION_REPLY_2, Framing::Offline, kHandshakeBudgetMs, reply2, reply2Length ) );

        // The server created the connection record, as UNVERIFIED_SENDER, before sending
        // that reply - so from here on its reliability layer is what receives from us.
    }

private:
    RNS2_Berkley m_socket;
    SystemAddress m_serverAddress;
};

} // namespace

TEST_CASE( "A truncated connection request from an unverified sender draws no reply", "[network]" )
{
    WinsockFixture winsock;
    PeerScope peers;

    RakPeerInterface* server = peers.Server( kServerPort );

    const SystemAddress serverAddress( "127.0.0.1", kServerPort );
    RawSystem rawSystem( serverAddress );
    rawSystem.CompleteOfflineHandshake();

    // One byte: the message id and nothing else. The writer emits 18 bytes at minimum -
    // MessageID | RakNetGUID | RakNet::Time | doSecurity - so all three reads come up
    // short, and none of the three values the reader goes on to use was ever written.
    const unsigned char truncatedRequest = (unsigned char)ID_CONNECTION_REQUEST;

    BitStream datagram;
    WriteDatagramHeader( datagram, 0 );
    WriteUnreliableMessage( datagram, &truncatedRequest, 1 );
    rawSystem.Send( datagram );

    // The whole defect in one line: before the fix an ID_CONNECTION_REQUEST_ACCEPTED came
    // back, carrying an echoed timestamp read from nothing. Stated as "not that message"
    // rather than "no datagram at all" so that a Peer which one day answers a stranger
    // with something honest - an error, a disconnection - would not fail it.
    char reply[MAXIMUM_MTU_SIZE];
    int replyLength = 0;
    CHECK_FALSE( rawSystem.WaitForMessage( ID_CONNECTION_REQUEST_ACCEPTED, RawSystem::Framing::Connected, kReplyBudgetMs, reply, replyLength ) );

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
