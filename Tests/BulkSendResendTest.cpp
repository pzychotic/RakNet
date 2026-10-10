#include "ConnectionWaits.h"
#include "PeerScope.h"

#include "GetTime.h"
#include "MessageIdentifiers.h"
#include "PacketPriority.h"
#include "RakNetStatistics.h"
#include "RakNetTime.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>
#include <vector>

/*
Pins that a burst of Sends on a warm connection is never resent on a timeout when nothing
was lost.

One update cycle applies every Send queued since the last, and a burst queued faster than
the network thread applies it keeps that cycle applying it for tens of milliseconds. The
datagrams the cycle then sends are timed from when they leave, not from when the cycle
began, so none times out before its ack can arrive.

The connection first carries a warm-up transfer, so the send window is open at its cap and
the burst's first window goes out at once. A datagram the kernel drops is resent but never
arrives twice, so any byte the receiver ignores as a duplicate was resent although its
original arrived.
*/

using namespace RakNet;

namespace {

constexpr MessageID kMessageId = ID_USER_PACKET_ENUM + 1;

constexpr int kWarmUpMessages = 5000;
constexpr int kWarmUpBytes = 1024;

// Enough small messages that applying them all outlasts the retransmission timeout.
constexpr int kBurstMessages = 300000;
constexpr int kBurstBytes = 32;

// How many messages go out between two drains of the receiver while sending.
constexpr int kSendsPerDrain = 1024;

// Hang guard for one transfer, not a timeout to tune.
constexpr TimeMS kTransferBudgetMs = 60000;

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

/// Sends \a count reliable messages of \a bytes each as fast as Send takes them, and
/// returns once the receiver has Received every one, or false if it never does.
bool Transfer( RakPeerInterface* sender, RakPeerInterface* receiver, int count, int bytes )
{
    std::vector<char> message( (size_t)bytes, 'x' );
    message[0] = (char)kMessageId;
    const RakNetGUID target = receiver->GetMyGUID();
    const TimeMS deadline = GetTimeMS() + kTransferBudgetMs;
    int received = 0;

    for( int sent = 0; sent < count; ++sent )
    {
        if( sender->Send( message.data(), bytes, HIGH_PRIORITY, RELIABLE_ORDERED, 0, target, false ) == 0 )
        {
            return false;
        }
        if( sent % kSendsPerDrain == 0 && !DrainCounting( receiver, received ) )
        {
            return false;
        }
    }

    while( received < count )
    {
        const int before = received;
        if( !DrainCounting( receiver, received ) || ConnectionWaits::Expired( deadline ) )
        {
            return false;
        }
        if( received == before )
        {
            std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
        }
    }
    return true;
}

} // namespace

TEST_CASE( "A burst of Sends on a warm connection is not resent while its acks are on the way", "[network]" )
{
    PeerScope peers;
    RakPeerInterface* receiver = peers.Server( 0 );
    RakPeerInterface* sender = peers.Client();
    ConnectionWaits::ConnectAndWait( sender, receiver );
    const SystemAddress senderAddress = receiver->GetSystemAddressFromGuid( sender->GetMyGUID() );

    REQUIRE( Transfer( sender, receiver, kWarmUpMessages, kWarmUpBytes ) );
    RakNetStatistics before{};
    REQUIRE( receiver->GetStatistics( senderAddress, &before ) != nullptr );

    REQUIRE( Transfer( sender, receiver, kBurstMessages, kBurstBytes ) );
    RakNetStatistics after{};
    REQUIRE( receiver->GetStatistics( senderAddress, &after ) != nullptr );

    CHECK( after.runningTotal[USER_MESSAGE_BYTES_RECEIVED_IGNORED] - before.runningTotal[USER_MESSAGE_BYTES_RECEIVED_IGNORED] == 0 );
}
