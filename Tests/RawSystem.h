#pragma once

#include "ConnectionWaits.h"
#include "ReliabilityLayerHarness.h"

#include "BitStream.h"
#include "GetTime.h"
#include "MTUSize.h"
#include "MessageIdentifiers.h"
#include "RakNetSocket2.h"
#include "RakNetTypes.h"
#include "RakNetVersion.h"
#include "RakPeerInterface.h"
#include "SequenceRanges.h"
#include "SocketDefines.h"

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <vector>

/*
A System with no Peer behind it: a bound UDP socket that speaks RakNet's handshake by
hand, so a test can be the far side of a connection and send what a real Peer never
would. Reaching a state such as UNVERIFIED_SENDER means *being* a System rather than
driving one - a real Peer sends its own well-formed ID_CONNECTION_REQUEST the instant the
offline handshake completes, and there is no supported way to ask it for anything else.

Once the connection record exists, everything is framed the way
ReliabilityLayerHarness writes it.

It plays either side. Constructed with the server's address it is a client and opens the
handshake; constructed without one it is a server, answers a Peer's Connect, and learns
the Peer's address from the first datagram it receives.

It sends an ack only when SendAck is called, so a reliable send to it otherwise keeps the
Peer's record of it open until the record's timeout, 10 s in Release. Shorten it with
SetTimeoutTime on its address. A Half-open record has nothing reliable outstanding until
that send, so it can't time out sooner.
*/

namespace RawSystemHarness {

using namespace RakNet;
using ReliabilityLayerHarness::WireMessage;

// One handshake datagram out, one reply back, no retries; or the server's next update
// cycle reporting the result. Generous by two orders of magnitude, and a hang guard
// rather than a tuning knob.
constexpr int kHandshakeBudgetMs = 5000;

// The 16-byte cookie every offline handshake message carries. Its only declaration in
// Source/ is a file-static in RakPeer.cpp, so a test that speaks the handshake has to
// spell it out; kept byte for byte against that one.
const unsigned char OFFLINE_MESSAGE_DATA_ID[16] = { 0x00, 0xFF, 0xFF, 0x00, 0xFE, 0xFE, 0xFE, 0xFE, 0xFD, 0xFD, 0xFD, 0xFD, 0x12, 0x34, 0x56, 0x78 };

/// ID_CONNECTION_REQUEST: MessageID | RakNetGUID | RakNet::Time | doSecurity | password,
/// field for field as ProcessOfflineNetworkPacket writes it (RakPeer.cpp), and in that
/// order so a wire-format change shows up here as a diff.
inline void WriteConnectionRequest( BitStream& out, RakNetGUID senderGuid, RakNet::Time timestamp, const char* password = nullptr, int passwordLength = 0 )
{
    out.Write( (MessageID)ID_CONNECTION_REQUEST );
    out.Write( senderGuid );
    out.Write( timestamp );
    out.Write( (unsigned char)0 ); // doSecurity
    if( passwordLength > 0 )
        out.WriteAlignedBytes( (const unsigned char*)password, (unsigned int)passwordLength );
}

/// The header of a datagram that carries messages, as DatagramHeaderFormat::Deserialize
/// (ReliabilityLayer.cpp) reads it, leaving \a in at the first message. False for an ACK or
/// NAK datagram.
inline bool ReadDataDatagramHeader( BitStream& in, DatagramSequenceNumberType& datagramNumber )
{
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
    return in.Read( datagramNumber );
}

/// Whether a datagram is an ACK whose ranges cover \a datagramNumber, read as
/// ReliabilityLayer::HandleSocketReceiveFromConnectedPlayer reads one.
inline bool AckCovers( const char* data, int length, DatagramSequenceNumberType datagramNumber )
{
    BitStream in( (unsigned char*)data, (unsigned int)length, false );

    bool isValid = false, isACK = false, hasBAndAS = false;
    if( in.Read( isValid ) == false || in.Read( isACK ) == false || isACK == false )
        return false;
    if( in.Read( hasBAndAS ) == false )
        return false;
    in.AlignReadToByteBoundary();
#if INCLUDE_TIMESTAMP_WITH_DATAGRAMS == 1
    RakNet::TimeMS sourceSystemTime = 0;
    if( in.Read( sourceSystemTime ) == false )
        return false;
#endif
    if( hasBAndAS )
    {
        float AS = 0;
        if( in.Read( AS ) == false )
            return false;
    }

    SequenceRanges<DatagramSequenceNumberType> acked;
    if( acked.Deserialize( &in ) == false )
        return false;
    for( const auto& range : acked.Ranges() )
    {
        if( range.first <= datagramNumber && datagramNumber <= range.last )
            return true;
    }
    return false;
}

/// Whether any message in a datagram starts with \a messageId, mirroring
/// ReliabilityLayer::CreateInternalPacketFromBitStream far enough to walk from one
/// message to the next. False for an ACK or NAK datagram, which carries no message at all.
/// A split message is recognised by its first chunk, the one its id is in.
///
/// Needed because everything the far side sends once the connection record exists is
/// wrapped in this framing, and it coalesces: the message a test waits for may sit behind
/// a ping in the same datagram.
inline bool DatagramCarriesMessage( const char* data, int length, MessageID messageId )
{
    BitStream in( (unsigned char*)data, (unsigned int)length, false );

    DatagramSequenceNumberType datagramNumber;
    if( ReadDataDatagramHeader( in, datagramNumber ) == false )
        return false;

    for( ;; )
    {
        in.AlignReadToByteBoundary();
        if( in.GetNumberOfUnreadBits() == 0 )
            return false;

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

        SplitPacketIndexType splitPacketIndex = 0;
        if( hasSplitPacket )
        {
            SplitPacketIndexType splitPacketCount;
            SplitPacketIdType splitPacketId;
            if( in.ReadAlignedVar32( (char*)&splitPacketCount ) == false ||
                in.ReadAlignedVar16( (char*)&splitPacketId ) == false ||
                in.ReadAlignedVar32( (char*)&splitPacketIndex ) == false )
                return false;
        }

        const unsigned int dataBytes = (unsigned int)BITS_TO_BYTES( dataBitLength );
        if( in.GetNumberOfUnreadBits() < BYTES_TO_BITS( dataBytes ) )
            return false;

        MessageID received = 0;
        if( in.ReadAlignedBytes( &received, sizeof( received ) ) == false )
            return false;
        if( received == messageId && splitPacketIndex == 0 )
            return true;
        in.IgnoreBytes( dataBytes - 1 );
    }
}

/// A bound UDP socket that speaks RakNet's handshake by hand.
class RawSystem
{
public:
    /// A client of \a serverAddress. The port is OS-assigned; the server reads it off the
    /// datagram.
    RawSystem( const SystemAddress& serverAddress, uint64_t guid )
    : m_farAddress( serverAddress )
    , m_guid( guid )
    {
    }

    /// A server on an OS-assigned port, GetBoundPort, for a Peer to Connect to. The Peer's
    /// address is the one the first datagram received comes from.
    explicit RawSystem( uint64_t guid )
    : m_farAddress( UNASSIGNED_SYSTEM_ADDRESS )
    , m_guid( guid )
    {
    }

    void Send( const BitStream& datagram )
    {
        RNS2_SendParameters sendParameters;
        sendParameters.data = (char*)datagram.GetData();
        sendParameters.length = (int)datagram.GetNumberOfBytesUsed();
        sendParameters.systemAddress = m_farAddress;

        REQUIRE( m_socket.Get().Send( &sendParameters, _FILE_AND_LINE_ ) == (RNS2SendResult)sendParameters.length );
    }

    /// One connected datagram carrying \a messages, with the next datagram number, which
    /// is returned.
    DatagramSequenceNumberType SendMessages( const std::vector<WireMessage>& messages )
    {
        const DatagramSequenceNumberType datagramNumber = m_datagramNumber++;
        BitStream datagram;
        ReliabilityLayerHarness::WriteDatagramHeader( datagram, datagramNumber );
        for( const WireMessage& message : messages )
        {
            ReliabilityLayerHarness::WriteWireMessage( datagram, message );
        }
        Send( datagram );
        return datagramNumber;
    }

    /// One connected datagram carrying one unsplit RELIABLE message, with the next reliable
    /// message number. Returns the datagram number, which is what the far side acks.
    DatagramSequenceNumberType SendReliable( const BitStream& message )
    {
        WireMessage wire;
        wire.reliability = RELIABLE;
        wire.reliableMessageNumber = m_reliableMessageNumber++;
        wire.isSplit = false;
        wire.payloadData = message.GetData();
        wire.payloadBytes = (unsigned short)message.GetNumberOfBytesUsed();
        return SendMessages( { wire } );
    }

    /// An ACK datagram for \a datagramNumber, as ReliabilityLayer::SendACKs writes one,
    /// without the B and AS a far side asks for only with needsBAndAs.
    void SendAck( DatagramSequenceNumberType datagramNumber )
    {
        BitStream datagram;
        datagram.Write( true );  // isValid
        datagram.Write( true );  // isACK
        datagram.Write( false ); // hasBAndAS
        datagram.AlignWriteToByteBoundary();
#if INCLUDE_TIMESTAMP_WITH_DATAGRAMS == 1
        datagram.Write( (RakNet::TimeMS)0 );
#endif
        SequenceRanges<DatagramSequenceNumberType> acked;
        acked.Insert( datagramNumber );
        acked.Serialize( &datagram, BYTES_TO_BITS( MAXIMUM_MTU_SIZE ), false );
        Send( datagram );
    }

    /// The datagram number of the last datagram a wait read. Only meaningful once a
    /// Connected wait has returned true.
    DatagramSequenceNumberType ReceivedDatagramNumber()
    {
        BitStream in( (unsigned char*)m_received, (unsigned int)m_receivedLength, false );
        DatagramSequenceNumberType datagramNumber;
        REQUIRE( ReadDataDatagramHeader( in, datagramNumber ) );
        return datagramNumber;
    }

    /// Whether an ack covering \a datagramNumber arrives before the budget is spent.
    /// Everything else is discarded.
    bool WaitForAck( DatagramSequenceNumberType datagramNumber, int millisecondsToWait )
    {
        const RakNet::TimeMS deadline = RakNet::GetTimeMS() + (RakNet::TimeMS)millisecondsToWait;
        while( !ConnectionWaits::Expired( deadline ) )
        {
            if( WaitForDatagram( (int)( deadline - RakNet::GetTimeMS() ) ) == false )
                return false;
            if( AckCovers( m_received, m_receivedLength, datagramNumber ) )
                return true;
        }
        return false;
    }

    /// Whether an ack covering \a datagramNumber is among the datagrams already waiting.
    /// Reads them all and does not block, so a caller can interleave it with driving a
    /// Peer.
    bool PollForAck( DatagramSequenceNumberType datagramNumber )
    {
        while( WaitForDatagram( 0 ) )
        {
            if( AckCovers( m_received, m_receivedLength, datagramNumber ) )
                return true;
        }
        return false;
    }

    /// One connected datagram carrying one unsplit UNRELIABLE message.
    void SendUnreliable( const BitStream& message )
    {
        WireMessage wire;
        wire.isSplit = false;
        wire.payloadData = message.GetData();
        wire.payloadBytes = (unsigned short)message.GetNumberOfBytesUsed();
        SendMessages( { wire } );
    }

    /// Where a message's id is to be found. An Offline message is written straight to the
    /// socket, so it is the datagram's first byte; a Connected one has come through a
    /// reliability layer, so it has to be decoded out of the framing.
    enum class Framing
    {
        Offline,
        Connected
    };

    /// Whether an offline message is the server refusing the handshake. Nothing follows a
    /// refusal, so a wait for the next handshake reply has already failed when one arrives.
    static bool IsHandshakeRefusal( MessageID messageId )
    {
        return messageId == ID_ALREADY_CONNECTED || messageId == ID_NO_FREE_INCOMING_CONNECTIONS ||
               messageId == ID_IP_RECENTLY_CONNECTED || messageId == ID_CONNECTION_BANNED ||
               messageId == ID_INCOMPATIBLE_PROTOCOL_VERSION;
    }

    /// Whether the next datagram carrying any of these message ids arrives before the
    /// budget is spent. Anything else is discarded and the wait continues: the server
    /// resends handshake replies, and none of them are what a caller here is waiting for.
    /// An Offline wait ends early, and false, on a handshake refusal.
    bool WaitForMessage( std::initializer_list<MessageID> messageIds, Framing framing, int millisecondsToWait )
    {
        const RakNet::TimeMS deadline = RakNet::GetTimeMS() + (RakNet::TimeMS)millisecondsToWait;
        m_received[0] = 0;
        m_receivedLength = 0;

        while( !ConnectionWaits::Expired( deadline ) )
        {
            const int remaining = (int)( deadline - RakNet::GetTimeMS() );
            if( WaitForDatagram( remaining ) == false )
                return false;

            for( MessageID messageId : messageIds )
            {
                if( framing == Framing::Offline ? (MessageID)m_received[0] == messageId
                                                : DatagramCarriesMessage( m_received, m_receivedLength, messageId ) )
                    return true;
            }
            if( framing == Framing::Offline && IsHandshakeRefusal( (MessageID)m_received[0] ) )
                return false;
        }

        return false;
    }

    bool WaitForMessage( MessageID messageId, Framing framing, int millisecondsToWait )
    {
        return WaitForMessage( { messageId }, framing, millisecondsToWait );
    }

    /// ID_OPEN_CONNECTION_REQUEST_1 and _2 and their replies, field for field as
    /// RakPeer::RunUpdateCycle and ProcessOfflineNetworkPacket write and read them, and in
    /// that order so a wire-format change shows up here as a diff.
    ///
    /// The server creates the connection record, as UNVERIFIED_SENDER, before it sends the
    /// second reply - so from here on its reliability layer is what receives from us, and
    /// this System is Half-open.
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

        REQUIRE( WaitForMessage( ID_OPEN_CONNECTION_REPLY_1, Framing::Offline, kHandshakeBudgetMs ) );

        BitStream reply1Stream( (unsigned char*)m_received, (unsigned int)m_receivedLength, false );
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
        request2.Write( m_farAddress ); // Binding address: the address being connected to
        request2.Write( mtu );
        request2.Write( RakNetGUID( m_guid ) );
        Send( request2 );

        // A record the server still holds for this port under another GUID is refused here
        // with ID_ALREADY_CONNECTED, so the port is part of the report.
        const bool replied = WaitForMessage( ID_OPEN_CONNECTION_REPLY_2, Framing::Offline, kHandshakeBudgetMs );
        INFO( "from port " << GetBoundPort() << ", the last offline message id was " << (int)(unsigned char)m_received[0] );
        REQUIRE( replied );
    }

    /// The server's half of CompleteOfflineHandshake: ID_OPEN_CONNECTION_REQUEST_1 and _2
    /// from a Peer that called Connect on GetBoundPort, and the replies, field for field as
    /// ProcessOfflineNetworkPacket reads and writes them (RakPeer.cpp).
    ///
    /// The Peer creates its connection record, as REQUESTED_CONNECTION, when the second
    /// reply arrives, and sends ID_CONNECTION_REQUEST RELIABLE at once.
    void AnswerOfflineHandshake()
    {
        REQUIRE( WaitForMessage( ID_OPEN_CONNECTION_REQUEST_1, Framing::Offline, kHandshakeBudgetMs ) );

        // The request's length is the Peer's MTU probe.
        const uint16_t mtu = (uint16_t)( m_receivedLength + UDP_HEADER_SIZE > MAXIMUM_MTU_SIZE ? MAXIMUM_MTU_SIZE : m_receivedLength + UDP_HEADER_SIZE );

        BitStream reply1;
        reply1.Write( (MessageID)ID_OPEN_CONNECTION_REPLY_1 );
        reply1.WriteAlignedBytes( (const unsigned char*)OFFLINE_MESSAGE_DATA_ID, sizeof( OFFLINE_MESSAGE_DATA_ID ) );
        reply1.Write( RakNetGUID( m_guid ) );
        reply1.Write( (unsigned char)0 ); // HasCookie: LIBCAT_SECURITY never compiles here
        reply1.Write( mtu );
        reply1.PadWithZeroToByteLength( mtu - reply1.GetNumberOfBytesUsed() );
        Send( reply1 );

        REQUIRE( WaitForMessage( ID_OPEN_CONNECTION_REQUEST_2, Framing::Offline, kHandshakeBudgetMs ) );

        BitStream request2( (unsigned char*)m_received, (unsigned int)m_receivedLength, false );
        request2.IgnoreBytes( sizeof( MessageID ) );
        request2.IgnoreBytes( sizeof( OFFLINE_MESSAGE_DATA_ID ) );

        SystemAddress bindingAddress;
        uint16_t requestedMtu = 0;
        RakNetGUID peerGuid;
        REQUIRE( request2.Read( bindingAddress ) );
        REQUIRE( request2.Read( requestedMtu ) );
        REQUIRE( request2.Read( peerGuid ) );

        BitStream reply2;
        reply2.Write( (MessageID)ID_OPEN_CONNECTION_REPLY_2 );
        reply2.WriteAlignedBytes( (const unsigned char*)OFFLINE_MESSAGE_DATA_ID, sizeof( OFFLINE_MESSAGE_DATA_ID ) );
        reply2.Write( RakNetGUID( m_guid ) );
        reply2.Write( m_farAddress );
        reply2.Write( requestedMtu );
        reply2.Write( false ); // requiresSecurityOfThisClient
        Send( reply2 );
    }

    /// CompleteOfflineHandshake, then everything that takes the server's record of this
    /// System to CONNECTED: ID_CONNECTION_REQUEST, its acceptance, and
    /// ID_NEW_INCOMING_CONNECTION, field for field as RakPeer writes and reads them. Sent
    /// UNRELIABLE, which the server does not check, so nothing here waits for an ack.
    ///
    /// Returns once \a server reports this System's ID_NEW_INCOMING_CONNECTION to its
    /// application, and every Message queued ahead of it is gone.
    void CompleteConnection( RakPeerInterface* server )
    {
        CompleteOfflineHandshake();
        CompleteConnectionRequest();

        REQUIRE( ConnectionWaits::WaitForMessage( server, ID_NEW_INCOMING_CONNECTION, kHandshakeBudgetMs,
                                                  [this]( const Packet& packet ) { return packet.guid == RakNetGUID( m_guid ); } ) );
    }

    /// The address \a server holds this System's record under, once the record exists.
    ///
    /// The server sends ID_OPEN_CONNECTION_REPLY_2 during an update cycle and publishes the
    /// record only at the cycle's end, so after CompleteOfflineHandshake returns the record
    /// may not be visible yet.
    SystemAddress WaitForServerRecord( RakPeerInterface* server )
    {
        SystemAddress address = UNASSIGNED_SYSTEM_ADDRESS;
        REQUIRE( ConnectionWaits::WaitUntil(
            [&] {
                address = server->GetSystemAddressFromGuid( RakNetGUID( m_guid ) );
                return address != UNASSIGNED_SYSTEM_ADDRESS;
            },
            kHandshakeBudgetMs ) );
        return address;
    }

    /// What CompleteConnection sends after CompleteOfflineHandshake, without its wait on
    /// the server, for a caller that wants the Half-open record to exist a while before
    /// it goes on.
    void CompleteConnectionRequest()
    {
        SendConnectionRequest( nullptr, 0 );

        REQUIRE( WaitForMessage( ID_CONNECTION_REQUEST_ACCEPTED, Framing::Connected, kHandshakeBudgetMs ) );

        BitStream newIncoming;
        newIncoming.Write( (MessageID)ID_NEW_INCOMING_CONNECTION );
        newIncoming.Write( m_farAddress );
        for( unsigned int i = 0; i < MAXIMUM_NUMBER_OF_INTERNAL_IDS; i++ )
            newIncoming.Write( UNASSIGNED_SYSTEM_ADDRESS );
        newIncoming.Write( RakNet::GetTime() ); // sendPingTime
        newIncoming.Write( RakNet::GetTime() ); // sendPongTime
        SendUnreliable( newIncoming );
    }

    /// ID_CONNECTION_REQUEST carrying \a password, field for field as RakPeer writes it, and
    /// nothing after. Sent UNRELIABLE, so nothing waits for an ack.
    void SendConnectionRequest( const char* password, int passwordLength )
    {
        BitStream request;
        WriteConnectionRequest( request, RakNetGUID( m_guid ), RakNet::GetTime(), password, passwordLength );
        SendUnreliable( request );
    }

    /// The length of the datagram SendJunkDatagram sends, which is how a datagram handler
    /// tells it apart.
    static constexpr int kJunkDatagramBytes = 2;

    /// Two bytes that are no RakNet message: ProcessNetworkPacket treats them as offline
    /// and discards them.
    void SendJunkDatagram()
    {
        BitStream junk;
        junk.Write( (MessageID)ID_USER_PACKET_ENUM );
        junk.Write( (MessageID)0 );
        REQUIRE( junk.GetNumberOfBytesUsed() == kJunkDatagramBytes );
        Send( junk );
    }

    /// An ID_UNCONNECTED_PING, field for field as RakPeer::Ping writes it. The Peer passes
    /// it to Receive.
    void SendUnconnectedPing()
    {
        BitStream ping;
        ping.Write( (MessageID)ID_UNCONNECTED_PING );
        ping.Write( RakNet::GetTime() );
        ping.WriteAlignedBytes( OFFLINE_MESSAGE_DATA_ID, sizeof( OFFLINE_MESSAGE_DATA_ID ) );
        ping.Write( RakNetGUID( m_guid ) );
        Send( ping );
    }

    /// The OS-assigned port, which is how the server knows this System.
    unsigned short GetBoundPort() const { return m_socket.Get().GetBoundAddress().GetPort(); }

private:
    /// The next datagram to arrive, into m_received, or false once millisecondsToWait is
    /// spent.
    bool WaitForDatagram( int millisecondsToWait )
    {
        const RNS2Socket socket = m_socket.Get().GetSocket();

        timeval timeout;
        timeout.tv_sec = (long)( millisecondsToWait / 1000 );
        timeout.tv_usec = (long)( ( millisecondsToWait % 1000 ) * 1000 );

        fd_set readable;
        FD_ZERO( &readable );
        FD_SET( socket, &readable );

        if( select__( (int)socket + 1, &readable, 0, 0, &timeout ) <= 0 )
            return false;

        sockaddr_storage from;
        socklen_t fromLength = sizeof( from );
        const int received = recvfrom__( socket, m_received, sizeof( m_received ), 0, (sockaddr*)&from, &fromLength );
        if( received <= 0 )
            return false;

        // The socket is bound on loopback, so the far side is too. Binding sends the socket
        // four zero bytes from itself (RNS2_Berkley::BindShared), which are not the far side.
        if( m_farAddress == UNASSIGNED_SYSTEM_ADDRESS && from.ss_family == AF_INET )
        {
            const unsigned short fromPort = ntohs( ( (const sockaddr_in*)&from )->sin_port );
            if( fromPort != GetBoundPort() )
                m_farAddress = SystemAddress( "127.0.0.1", fromPort );
        }

        m_receivedLength = received;
        return true;
    }

    ReliabilityLayerHarness::BoundSocket m_socket;
    SystemAddress m_farAddress;
    uint64_t m_guid;
    DatagramSequenceNumberType m_datagramNumber = 0;
    MessageNumberType m_reliableMessageNumber = 0;
    // The last datagram WaitForDatagram read.
    char m_received[MAXIMUM_MTU_SIZE];
    int m_receivedLength = 0;
};

} // namespace RawSystemHarness
