#include "ConnectionWaits.h"
#include "GetTime.h"
#include "MessageIdentifiers.h"
#include "PeerScope.h"
#include "RakNetStringMakers.h"
#include "RakNetTypes.h"
#include "RakPeerInterface.h"
#include "RawSystem.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

/*
Pins that asking to close a closing connection record again never stops this Peer
acknowledging a close the System asked for, and that every connection the application was
told had opened gets exactly one end message (GLOSSARY.md).

B receives A's disconnection notification and owes A an ack for it. The view can be one
update cycle old (ADR-0007), so B's application can still ask to close the connection at that
moment, with or without a notification. Either way B still sends its ack, and A's close ends
at once, not at A's timeout.

The end message of a silent close is decided when the close is applied, from the record's
mode, so a close the System started still ends with ID_DISCONNECTION_NOTIFICATION, a second
silent close adds nothing, and a connection that never opened ends with none.

RakPeerInterface functions explicitly tested:

    CloseConnection
*/

using namespace RakNet;
using RawSystemHarness::RawSystem;

namespace {

// Ports no other test uses.
constexpr unsigned short kPortA = 32500;
constexpr unsigned short kPortB = 32501;

// Hang guard for the connect and for B to start closing, not a settle time.
constexpr TimeMS kWaitBudgetMs = 10000;

// Many times the 10 ms an ack is held, and a few round trips on loopback.
constexpr TimeMS kQuietMs = 200;

// Far inside the 10 s Release and 30 s Debug timeout a close ends at when its notification
// is never acked, and far outside a close on loopback, which takes tens of milliseconds.
constexpr TimeMS kCloseBudgetMs = 1000;

// Short enough that B's record of a System that never acks dies well inside kCloseBudgetMs.
// The record has nothing reliable outstanding until B's notification goes out, so it can't
// time out before the close is applied.
constexpr TimeMS kShortTimeoutMs = 300;

const SystemAddress kAddressA( "127.0.0.1", kPortA );
const SystemAddress kAddressB( "127.0.0.1", kPortB );

// The Messages a Peer has handed out that end, or fail to open, one System's connection.
struct EndMessages
{
    int lost = 0;
    int notification = 0;
    int attemptFailed = 0;
};

// Receives everything queued on peer, counting what ends system's connection.
void DrainCounting( RakPeerInterface* peer, const SystemAddress& system, EndMessages& counts )
{
    for( Packet* packet = peer->Receive(); packet; peer->DeallocatePacket( packet ), packet = peer->Receive() )
    {
        if( packet->systemAddress != system )
            continue;
        if( packet->data[0] == ID_CONNECTION_LOST )
            counts.lost++;
        else if( packet->data[0] == ID_DISCONNECTION_NOTIFICATION )
            counts.notification++;
        else if( packet->data[0] == ID_CONNECTION_ATTEMPT_FAILED )
            counts.attemptFailed++;
    }
}

// Drains peer, counting, for kQuietMs, so a second end message has time to arrive.
void DrainCountingWhileQuiet( RakPeerInterface* peer, const SystemAddress& system, EndMessages& counts )
{
    const TimeMS deadline = GetTimeMS() + kQuietMs;
    while( ConnectionWaits::Expired( deadline ) == false )
    {
        DrainCounting( peer, system, counts );
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }
    DrainCounting( peer, system, counts );
}

// A silent close doesn't wake peer's network thread, and its next update cycle can come as
// late as an ack's hold running out. An IMMEDIATE_PRIORITY send wakes it now; the peer
// refuses the send itself, as the record is closing or gone.
void Wake( RakPeerInterface* peer, const SystemAddress& system )
{
    const unsigned char probe = ID_USER_PACKET_ENUM;
    peer->Send( reinterpret_cast<const char*>( &probe ), 1, IMMEDIATE_PRIORITY, RELIABLE_ORDERED, 0, system, false );
}

// Connects A to B and lets the connect's traffic, its pings and their acks, die down.
void Connect( RakPeerInterface* a, RakPeerInterface* b )
{
    REQUIRE( a->Connect( "127.0.0.1", kPortB, 0, 0 ) == CONNECTION_ATTEMPT_STARTED );
    const TimeMS deadline = GetTimeMS() + kWaitBudgetMs;
    while( ( a->GetConnectionState( kAddressB ) != IS_CONNECTED || b->GetConnectionState( kAddressA ) != IS_CONNECTED ) &&
           ConnectionWaits::Expired( deadline ) == false )
        std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
    REQUIRE( a->GetConnectionState( kAddressB ) == IS_CONNECTED );
    REQUIRE( b->GetConnectionState( kAddressA ) == IS_CONNECTED );
    std::this_thread::sleep_for( std::chrono::milliseconds( kQuietMs ) );
    ConnectionWaits::Drain( a );
    ConnectionWaits::Drain( b );
}

// Connects A to B, then has A close and spins until B is acknowledging that close.
void ConnectThenCloseFromA( RakPeerInterface* a, RakPeerInterface* b )
{
    // Connect lets the connect's traffic die down: an ack B still holds for any of it would
    // go out with the one for A's notification, early.
    Connect( a, b );

    a->CloseConnection( kAddressB, true );

    // No sleep: B's close has to land while B still holds the ack back.
    const TimeMS deadline = GetTimeMS() + kWaitBudgetMs;
    while( b->GetConnectionState( kAddressA ) != IS_DISCONNECTING && ConnectionWaits::Expired( deadline ) == false )
    {
    }
    REQUIRE( b->GetConnectionState( kAddressA ) == IS_DISCONNECTING );
}

} // namespace

TEST_CASE( "Closing a connection the System is already closing still acks the System's notification", "[network]" )
{
    PeerScope peers;
    RakPeerInterface* a = peers.Server( kPortA );
    RakPeerInterface* b = peers.Server( kPortB );

    ConnectThenCloseFromA( a, b );
    // IMMEDIATE_PRIORITY wakes B's network thread now. Left to its own pace, B's next
    // update cycle comes as the ack's hold runs out and sends the ack before the close.
    b->CloseConnection( kAddressA, true, 0, IMMEDIATE_PRIORITY );

    const TimeMS closeStarted = GetTimeMS();
    const TimeMS deadline = closeStarted + kCloseBudgetMs;
    while( a->GetConnectionState( kAddressB ) != IS_NOT_CONNECTED && ConnectionWaits::Expired( deadline ) == false )
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );

    INFO( "A's state toward B after " << ( GetTimeMS() - closeStarted ) << " ms" );
    CHECK( a->GetConnectionState( kAddressB ) == IS_NOT_CONNECTED );
}

TEST_CASE( "Silently closing a connection the System is already closing acks the System's notification and reports it once", "[network]" )
{
    PeerScope peers;
    RakPeerInterface* a = peers.Server( kPortA );
    RakPeerInterface* b = peers.Server( kPortB );

    ConnectThenCloseFromA( a, b );
    b->CloseConnection( kAddressA, false );
    Wake( b, kAddressA );

    const TimeMS closeStarted = GetTimeMS();
    const TimeMS deadline = closeStarted + kCloseBudgetMs;
    EndMessages fromA;
    while( ( a->GetConnectionState( kAddressB ) != IS_NOT_CONNECTED || b->GetConnectionState( kAddressA ) != IS_NOT_CONNECTED ) &&
           ConnectionWaits::Expired( deadline ) == false )
    {
        DrainCounting( b, kAddressA, fromA );
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }
    DrainCountingWhileQuiet( b, kAddressA, fromA );

    INFO( "States after " << ( GetTimeMS() - closeStarted ) << " ms" );
    CHECK( a->GetConnectionState( kAddressB ) == IS_NOT_CONNECTED );
    CHECK( b->GetConnectionState( kAddressA ) == IS_NOT_CONNECTED );
    CHECK( fromA.notification == 1 );
    CHECK( fromA.lost == 0 );
}

TEST_CASE( "Silently closing a connection this Peer is already closing reports it once", "[network]" )
{
    PeerScope peers;
    RakPeerInterface* a = peers.Server( kPortA );
    RakPeerInterface* b = peers.Server( kPortB );

    Connect( a, b );
    // A's end is gone, so B's notification is never acked and B's close waits on it.
    a->Shutdown( 0 );

    b->CloseConnection( kAddressA, true );
    TimeMS deadline = GetTimeMS() + kWaitBudgetMs;
    while( b->GetConnectionState( kAddressA ) != IS_DISCONNECTING && ConnectionWaits::Expired( deadline ) == false )
    {
    }
    REQUIRE( b->GetConnectionState( kAddressA ) == IS_DISCONNECTING );

    b->CloseConnection( kAddressA, false );
    Wake( b, kAddressA );

    const TimeMS closeStarted = GetTimeMS();
    deadline = closeStarted + kCloseBudgetMs;
    EndMessages fromA;
    while( ( b->GetConnectionState( kAddressA ) != IS_NOT_CONNECTED || fromA.notification == 0 ) && ConnectionWaits::Expired( deadline ) == false )
    {
        DrainCounting( b, kAddressA, fromA );
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }
    DrainCountingWhileQuiet( b, kAddressA, fromA );

    INFO( "B's state toward A after " << ( GetTimeMS() - closeStarted ) << " ms" );
    CHECK( b->GetConnectionState( kAddressA ) == IS_NOT_CONNECTED );
    CHECK( fromA.notification == 1 );
    CHECK( fromA.lost == 0 );
}

TEST_CASE( "Silently closing a connection twice reports it once", "[network]" )
{
    PeerScope peers;
    RakPeerInterface* a = peers.Server( kPortA );
    RakPeerInterface* b = peers.Server( kPortB );

    Connect( a, b );
    b->CloseConnection( kAddressA, false );
    b->CloseConnection( kAddressA, false );
    Wake( b, kAddressA );

    const TimeMS closeStarted = GetTimeMS();
    const TimeMS deadline = closeStarted + kCloseBudgetMs;
    EndMessages fromA;
    while( fromA.lost == 0 && ConnectionWaits::Expired( deadline ) == false )
    {
        DrainCounting( b, kAddressA, fromA );
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }
    DrainCountingWhileQuiet( b, kAddressA, fromA );

    INFO( "After " << ( GetTimeMS() - closeStarted ) << " ms" );
    CHECK( fromA.lost == 1 );
    CHECK( fromA.notification == 0 );
    CHECK( fromA.attemptFailed == 0 );
}

TEST_CASE( "Closing a connection that never opened reports nothing", "[network]" )
{
    PeerScope peers;
    RakPeerInterface* b = peers.Server( kPortB );

    // Completes the offline handshake and never sends ID_CONNECTION_REQUEST, so B holds a
    // half-open record of it, and never acks B's notification.
    RawSystem raw( kAddressB, 0x7474 );
    raw.CompleteOfflineHandshake();
    const SystemAddress rawAddress( "127.0.0.1", raw.GetBoundPort() );
    const TimeMS openDeadline = GetTimeMS() + kWaitBudgetMs;
    while( b->GetConnectionState( rawAddress ) != IS_CONNECTING && ConnectionWaits::Expired( openDeadline ) == false )
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    REQUIRE( b->GetConnectionState( rawAddress ) == IS_CONNECTING );

    b->SetTimeoutTime( kShortTimeoutMs, rawAddress );
    // IMMEDIATE_PRIORITY wakes B's network thread now.
    b->CloseConnection( rawAddress, true, 0, IMMEDIATE_PRIORITY );

    const TimeMS closeStarted = GetTimeMS();
    const TimeMS deadline = closeStarted + kCloseBudgetMs;
    EndMessages fromRaw;
    while( b->GetConnectionState( rawAddress ) != IS_NOT_CONNECTED && ConnectionWaits::Expired( deadline ) == false )
    {
        DrainCounting( b, rawAddress, fromRaw );
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }
    DrainCountingWhileQuiet( b, rawAddress, fromRaw );

    INFO( "B's state toward the RawSystem after " << ( GetTimeMS() - closeStarted ) << " ms" );
    CHECK( b->GetConnectionState( rawAddress ) == IS_NOT_CONNECTED );
    CHECK( fromRaw.notification == 0 );
    CHECK( fromRaw.lost == 0 );
    CHECK( fromRaw.attemptFailed == 0 );
}
