#include "BitStream.h"
#include "ConnectionWaits.h"
#include "GetTime.h"
#include "MessageIdentifiers.h"
#include "PeerScope.h"
#include "RakNetDefines.h"
#include "RakNetStringMakers.h"
#include "RakNetTypes.h"
#include "RakPeer.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

/*
Pins that a blocking Shutdown tells each connected System it is going.

A Shutdown with a block duration queues ID_DISCONNECTION_NOTIFICATION to every connection
record it finds, and waits for those records to close before it stops the network thread.
The System on the other end receives the notification, and Shutdown returns once it has
acknowledged, long before the block duration runs out.

Under RAKPEER_USER_THREADED the test runs every update cycle of the Peer it keeps, and
Shutdown runs the cycles of the Peer it shuts down while it waits.

RakPeerInterface functions explicitly tested:

    Shutdown
*/

using namespace RakNet;

namespace {

// Ports no other test uses.
constexpr unsigned short kPortA = 32600;
constexpr unsigned short kPortB = 32601;

// Hang guard for the connect and for the notification to arrive, not a settle time.
constexpr TimeMS kWaitBudgetMs = 10000;

// Many update cycles and loopback round trips, so the notification and its ack go out
// well inside it.
constexpr unsigned int kShutdownBlockMs = 500;

// A Shutdown that waited for the ack rather than for the whole block duration.
constexpr TimeMS kPromptShutdownMs = 400;

/// Runs one update cycle under RAKPEER_USER_THREADED.
void RunCycle( RakPeerInterface* peer )
{
#if RAKPEER_USER_THREADED == 1
    BitStream updateBitStream( MAXIMUM_MTU_SIZE );
    static_cast<RakPeer*>( peer )->RunUpdateCycle( updateBitStream );
#else
    (void)peer;
#endif
}

} // namespace

TEST_CASE( "A blocking Shutdown sends each connected System a disconnection notification", "[network]" )
{
    PeerScope peers;
    RakPeerInterface* a = peers.Server( kPortA );
    RakPeerInterface* b = peers.Server( kPortB );
    const SystemAddress addressA( "127.0.0.1", kPortA );
    const SystemAddress addressB( "127.0.0.1", kPortB );

    REQUIRE( a->Connect( "127.0.0.1", kPortB, 0, 0 ) == CONNECTION_ATTEMPT_STARTED );
    TimeMS deadline = GetTimeMS() + kWaitBudgetMs;
    while( ( a->GetConnectionState( addressB ) != IS_CONNECTED || b->GetConnectionState( addressA ) != IS_CONNECTED ) &&
           ConnectionWaits::Expired( deadline ) == false )
    {
        RunCycle( a );
        RunCycle( b );
        std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
    }
    REQUIRE( a->GetConnectionState( addressB ) == IS_CONNECTED );
    REQUIRE( b->GetConnectionState( addressA ) == IS_CONNECTED );
    ConnectionWaits::Drain( b );

    // B is pumped on its own thread while this one blocks in A's Shutdown.
    std::atomic<bool> stop{ false };
    std::atomic<bool> notified{ false };
    std::thread pumpB( [&] {
        while( stop == false )
        {
            RunCycle( b );
            for( Packet* packet = b->Receive(); packet != 0; packet = b->Receive() )
            {
                if( packet->data[0] == ID_DISCONNECTION_NOTIFICATION && packet->systemAddress == addressA )
                    notified = true;
                b->DeallocatePacket( packet );
            }
            std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
        }
    } );

    const TimeMS start = GetTimeMS();
    a->Shutdown( kShutdownBlockMs );
    const TimeMS took = GetTimeMS() - start;

    deadline = GetTimeMS() + kWaitBudgetMs;
    while( notified == false && ConnectionWaits::Expired( deadline ) == false )
        std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
    stop = true;
    pumpB.join();

    CHECK( notified );
    CHECK( took < kPromptShutdownMs );
}
