#include "ConnectionWaits.h"
#include "GetTime.h"
#include "MessageIdentifiers.h"
#include "PeerScope.h"
#include "RakNetStringMakers.h"
#include "RakNetTypes.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

/*
Pins that a blocking Shutdown tells each connected System it is going.

A Shutdown with a block duration queues ID_DISCONNECTION_NOTIFICATION to every connection
record it finds, and waits for those records to close before it stops the network thread.
The System on the other end receives the notification.

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
        std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
    REQUIRE( a->GetConnectionState( addressB ) == IS_CONNECTED );
    REQUIRE( b->GetConnectionState( addressA ) == IS_CONNECTED );
    ConnectionWaits::Drain( b );

    a->Shutdown( kShutdownBlockMs );

    bool notified = false;
    deadline = GetTimeMS() + kWaitBudgetMs;
    while( notified == false && ConnectionWaits::Expired( deadline ) == false )
    {
        Packet* packet = b->Receive();
        if( packet == 0 )
        {
            std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
            continue;
        }
        if( packet->data[0] == ID_DISCONNECTION_NOTIFICATION && packet->systemAddress == addressA )
            notified = true;
        b->DeallocatePacket( packet );
    }

    CHECK( notified );
}
