#pragma once

#include "ReliabilityLayerHarness.h"

#include "BitStream.h"
#include "GetTime.h"
#include "MTUSize.h"
#include "MessageIdentifiers.h"
#include "RakNetSocket2.h"
#include "RakNetTypes.h"
#include "RakNetVersion.h"
#include "SocketDefines.h"
#include "WSAStartupSingleton.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <vector>

/*
A System with no Peer behind it: a bound UDP socket that speaks RakNet's handshake by
hand, so a test can be the far side of a connection and send what a real Peer never
would. Reaching a state such as UNVERIFIED_SENDER means *being* a System rather than
driving one - a real Peer sends its own well-formed ID_CONNECTION_REQUEST the instant the
offline handshake completes, and there is no supported way to ask it for anything else.

Once the connection record exists, everything is framed the way
ReliabilityLayerHarness writes it.

It never sends acks, so a reliable send to it keeps the Peer's record of it open until the
record's timeout, 10 s in Release. Shorten it with SetTimeoutTime on its address. A Half-open
record has nothing reliable outstanding until that send, so it can't time out sooner.
*/

namespace RawSystemHarness {

using namespace RakNet;
using ReliabilityLayerHarness::WireMessage;

// One handshake datagram out, one reply back, no retries. Generous by two orders of
// magnitude, and a hang guard rather than a tuning knob.
constexpr int kHandshakeBudgetMs = 5000;

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
    RawSystem( const SystemAddress& serverAddress, uint64_t guid )
    : m_serverAddress( serverAddress )
    , m_guid( guid )
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

    /// One connected datagram carrying \a messages, with the next datagram number.
    void SendMessages( const std::vector<WireMessage>& messages )
    {
        BitStream datagram;
        ReliabilityLayerHarness::WriteDatagramHeader( datagram, m_datagramNumber++ );
        for( const WireMessage& message : messages )
        {
            ReliabilityLayerHarness::WriteWireMessage( datagram, message );
        }
        Send( datagram );
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

    /// Whether an offline message is the server refusing the handshake. Nothing follows a
    /// refusal, so a wait for the next handshake reply has already failed when one arrives.
    static bool IsHandshakeRefusal( MessageID messageId )
    {
        return messageId == ID_ALREADY_CONNECTED || messageId == ID_NO_FREE_INCOMING_CONNECTIONS ||
               messageId == ID_IP_RECENTLY_CONNECTED || messageId == ID_CONNECTION_BANNED ||
               messageId == ID_INCOMPATIBLE_PROTOCOL_VERSION;
    }

    /// The next datagram carrying this message id, or false once the budget is spent.
    /// Anything else is discarded and the wait continues: the server resends handshake
    /// replies, and none of them are what a caller here is waiting for. An Offline wait
    /// ends early, and false, on a handshake refusal, which is left in dataOut.
    bool WaitForMessage( MessageID messageId, Framing framing, int millisecondsToWait, char* dataOut, int& lengthOut )
    {
        const RakNet::TimeMS deadline = RakNet::GetTimeMS() + (RakNet::TimeMS)millisecondsToWait;

        while( RakNet::GetTimeMS() < deadline )
        {
            const int remaining = (int)( deadline - RakNet::GetTimeMS() );
            if( WaitForDatagram( remaining, dataOut, lengthOut ) == false )
                return false;

            if( framing == Framing::Offline )
            {
                if( (unsigned char)dataOut[0] == messageId )
                    return true;
                if( IsHandshakeRefusal( (MessageID)dataOut[0] ) )
                    return false;
            }
            else if( DatagramCarriesMessage( dataOut, lengthOut, messageId ) )
            {
                return true;
            }
        }

        return false;
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
        request2.Write( RakNetGUID( m_guid ) );
        Send( request2 );

        // A record the server still holds for this port under another GUID is refused here
        // with ID_ALREADY_CONNECTED, so the port is part of the report.
        char reply2[MAXIMUM_MTU_SIZE] = {};
        int reply2Length = 0;
        const bool replied = WaitForMessage( ID_OPEN_CONNECTION_REPLY_2, Framing::Offline, kHandshakeBudgetMs, reply2, reply2Length );
        INFO( "from port " << GetBoundPort() << ", the last offline message id was " << (int)(unsigned char)reply2[0] );
        REQUIRE( replied );
    }

    /// CompleteOfflineHandshake, then everything that takes the server's record of this
    /// System to CONNECTED: ID_CONNECTION_REQUEST, its acceptance, and
    /// ID_NEW_INCOMING_CONNECTION, field for field as RakPeer writes and reads them. Sent
    /// UNRELIABLE, which the server does not check, so nothing here waits for an ack.
    ///
    /// The server reports ID_NEW_INCOMING_CONNECTION to its application once this lands;
    /// waiting for that is the caller's, since only the caller holds the server.
    void CompleteConnection()
    {
        CompleteOfflineHandshake();
        CompleteConnectionRequest();
    }

    /// The part of CompleteConnection after CompleteOfflineHandshake, for a caller that
    /// wants the Half-open record to exist a while before it goes on.
    void CompleteConnectionRequest()
    {
        SendConnectionRequest( nullptr, 0 );

        char accepted[MAXIMUM_MTU_SIZE];
        int acceptedLength = 0;
        REQUIRE( WaitForMessage( ID_CONNECTION_REQUEST_ACCEPTED, Framing::Connected, kHandshakeBudgetMs, accepted, acceptedLength ) );

        BitStream newIncoming;
        newIncoming.Write( (MessageID)ID_NEW_INCOMING_CONNECTION );
        newIncoming.Write( m_serverAddress );
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
        request.Write( (MessageID)ID_CONNECTION_REQUEST );
        request.Write( RakNetGUID( m_guid ) );
        request.Write( RakNet::GetTime() );
        request.Write( (unsigned char)0 ); // doSecurity
        if( passwordLength > 0 )
            request.WriteAlignedBytes( (const unsigned char*)password, (unsigned int)passwordLength );
        SendUnreliable( request );
    }

    /// The OS-assigned port, which is how the server knows this System.
    unsigned short GetBoundPort() const { return m_socket.GetBoundAddress().GetPort(); }

private:
    RNS2_Berkley m_socket;
    SystemAddress m_serverAddress;
    uint64_t m_guid;
    DatagramSequenceNumberType m_datagramNumber = 0;
};

} // namespace RawSystemHarness
