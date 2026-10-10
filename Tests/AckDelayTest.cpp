#include "ConnectionWaits.h"
#include "PeerScope.h"

#include "GetTime.h"
#include "MessageIdentifiers.h"
#include "PacketPriority.h"
#include "RakNetStatistics.h"
#include "RakNetTime.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

/*
Pins when a receiving Peer acknowledges datagrams (the Ack delay).

A lone small reliable message is acknowledged in the update cycle that handled it. Nothing
else arrives to carry its ack sooner, so the sender's ID_SND_RECEIPT_ACKED, its
retransmission timeout and its small-message throughput would all wait on a hold.

A bulk transfer is acknowledged about once a millisecond, not once per datagram or per
update cycle. Each ack datagram costs both Peers' network threads a send or a receive, and
on loopback the sender's network thread is the bottleneck.
*/

using namespace RakNet;

namespace {

constexpr MessageID kMessageId = ID_USER_PACKET_ENUM + 1;
constexpr int kMessages = 20;
constexpr int kMessageBytes = 32;

// One loopback round trip is about a millisecond, and the ack gap adds at most another.
// An ack held for 10 ms would put the median past this.
constexpr TimeUS kMedianBudgetUs = 5000;

// Hang guard for one receipt, not a timeout to tune.
constexpr TimeMS kReceiptBudgetMs = 5000;

constexpr int kBulkMessages = 20000;
constexpr int kBulkMessageBytes = 4096;

// An ack datagram is at least this many bytes: its header byte, the time it carries and one
// range.
constexpr uint64_t kMinAckDatagramBytes = 8;

// Each 4 KB Message is three datagrams. Acknowledged once a millisecond, the transfer takes
// far fewer ack datagrams than this allows; one per update cycle takes more.
constexpr int kMessagesPerAckDatagram = 10;

// Hang guard for the bulk transfer, not a timeout to tune.
constexpr TimeMS kBulkBudgetMs = 60000;

void DrainAll( RakPeerInterface* peer )
{
    for( Packet* packet = peer->Receive(); packet != nullptr; packet = peer->Receive() )
    {
        peer->DeallocatePacket( packet );
    }
}

/// Receives and deallocates everything queued on \a receiver, adding the test's messages
/// to \a received. Returns false if the connection was lost.
bool DrainCounting( RakPeerInterface* receiver, int& received )
{
    for( Packet* packet = receiver->Receive(); packet != nullptr; packet = receiver->Receive() )
    {
        const MessageID id = packet->data[0];
        receiver->DeallocatePacket( packet );
        if( id == kMessageId )
        {
            ++received;
        }
        else if( id == ID_CONNECTION_LOST || id == ID_DISCONNECTION_NOTIFICATION )
        {
            return false;
        }
    }
    return true;
}

/// Polls \a sender until ID_SND_RECEIPT_ACKED for \a receipt arrives. Returns false if it
/// does not arrive within the budget, or a receipt loss arrives instead.
bool WaitForReceiptAcked( RakPeerInterface* sender, uint32_t receipt )
{
    const TimeMS deadline = GetTimeMS() + kReceiptBudgetMs;
    while( !ConnectionWaits::Expired( deadline ) )
    {
        for( Packet* packet = sender->Receive(); packet != nullptr; packet = sender->Receive() )
        {
            const MessageID id = packet->data[0];
            uint32_t packetReceipt = 0;
            if( ( id == ID_SND_RECEIPT_ACKED || id == ID_SND_RECEIPT_LOSS ) && packet->length >= 5 )
            {
                memcpy( &packetReceipt, packet->data + 1, sizeof( packetReceipt ) );
            }
            sender->DeallocatePacket( packet );
            if( packetReceipt == receipt && id == ID_SND_RECEIPT_ACKED )
            {
                return true;
            }
            if( packetReceipt == receipt && id == ID_SND_RECEIPT_LOSS )
            {
                return false;
            }
        }
        std::this_thread::yield();
    }
    return false;
}

} // namespace

TEST_CASE( "A lone small reliable message is acknowledged without an ack delay", "[network]" )
{
    PeerScope peers;
    RakPeerInterface* receiver = peers.Server( 0 );
    RakPeerInterface* sender = peers.Client();
    ConnectionWaits::ConnectAndWait( sender, receiver );
    DrainAll( sender );

    char message[kMessageBytes] = {};
    message[0] = (char)kMessageId;
    const RakNetGUID target = receiver->GetMyGUID();

    std::vector<TimeUS> roundTrips;
    for( int i = 0; i < kMessages; ++i )
    {
        const TimeUS sentAt = GetTimeUS();
        const uint32_t receipt = sender->Send( message, kMessageBytes, IMMEDIATE_PRIORITY, RELIABLE_WITH_ACK_RECEIPT, 0, target, false );
        REQUIRE( receipt != 0 );
        REQUIRE( WaitForReceiptAcked( sender, receipt ) );
        roundTrips.push_back( GetTimeUS() - sentAt );
        DrainAll( receiver );
    }

    std::sort( roundTrips.begin(), roundTrips.end() );
    const TimeUS median = roundTrips[roundTrips.size() / 2];
    INFO( "median " << median << " us, min " << roundTrips.front() << " us, max " << roundTrips.back() << " us" );
    CHECK( median < kMedianBudgetUs );
}

TEST_CASE( "A bulk transfer is acknowledged far less often than once per Message", "[network]" )
{
    PeerScope peers;
    RakPeerInterface* receiver = peers.Server( 0 );
    RakPeerInterface* sender = peers.Client();
    ConnectionWaits::ConnectAndWait( sender, receiver );
    const SystemAddress senderAddress = receiver->GetSystemAddressFromGuid( sender->GetMyGUID() );
    const RakNetGUID target = receiver->GetMyGUID();

    std::vector<char> message( (size_t)kBulkMessageBytes, 'x' );
    message[0] = (char)kMessageId;

    RakNetStatistics before{};
    REQUIRE( receiver->GetStatistics( senderAddress, &before ) != nullptr );

    const TimeMS deadline = GetTimeMS() + kBulkBudgetMs;
    int received = 0;
    for( int sent = 0; sent < kBulkMessages; ++sent )
    {
        REQUIRE( sender->Send( message.data(), kBulkMessageBytes, HIGH_PRIORITY, RELIABLE_ORDERED, 0, target, false ) != 0 );
        if( sent % 1024 == 0 )
        {
            REQUIRE( DrainCounting( receiver, received ) );
        }
    }
    while( received < kBulkMessages )
    {
        const int beforeCount = received;
        REQUIRE( DrainCounting( receiver, received ) );
        REQUIRE_FALSE( ConnectionWaits::Expired( deadline ) );
        if( received == beforeCount )
        {
            std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
        }
    }

    RakNetStatistics after{};
    REQUIRE( receiver->GetStatistics( senderAddress, &after ) != nullptr );
    const uint64_t receiverBytesSent = after.runningTotal[ACTUAL_BYTES_SENT] - before.runningTotal[ACTUAL_BYTES_SENT];
    INFO( "the receiver sent " << receiverBytesSent << " bytes for " << kBulkMessages << " Messages" );
    CHECK( receiverBytesSent / kMinAckDatagramBytes < (uint64_t)( kBulkMessages / kMessagesPerAckDatagram ) );
}
