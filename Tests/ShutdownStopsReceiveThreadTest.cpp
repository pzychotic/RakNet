#include "BitStream.h"
#include "ConnectionWaits.h"
#include "GetTime.h"
#include "MessageIdentifiers.h"
#include "PeerScope.h"
#include "RawSystem.h"
#include "RakPeer.h"
#include "RakNetSocket2.h"
#include "RakNetTypes.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

/*
Pins that Shutdown frees a socket only once its receive thread has exited, even when the
datagram it sends the socket to wake that thread never arrives.

The receive thread blocks in recvfrom. To stop it, Shutdown sends the socket a datagram of
its own, and the thread checks its stop flag when recvfrom returns. Here a
SocketLayerOverride swallows every send to the Peer's own bound address, so that datagram
is never sent. Shutdown then has to rely on the receive thread's wait for a datagram, which
times out, to stop the thread.

If Shutdown stops waiting before the thread exits, it frees the socket under a thread still
blocked in recvfrom. On Linux closing the socket doesn't wake that recvfrom, and the port
stays bound to it, so the datagram this test sends afterwards wakes the thread into freed
memory. AddressSanitizer
reports it. Without ASan the test can still pass, so it fails reliably only in an ASan
build.

Startup sends the socket a datagram of its own to test the bind, and the receive thread may
not have read it yet when Startup returns. If it is still queued at Shutdown, it wakes the
thread as the lost datagram would have. So the test first waits for the Peer to answer a
ping: the socket reads in order, so the bind test datagram is gone by then.

There is no timing assertion: the guarantee is that the thread exits before the socket goes,
not how soon.

RakPeerInterface functions explicitly tested:

    Shutdown
*/

using namespace RakNet;

namespace {

// Ports no other test uses.
constexpr unsigned short kPeerPort = 32700;
constexpr unsigned short kPingerPort = 32701;

// Hang guard for the pong, not a settle time.
constexpr TimeMS kWaitBudgetMs = 10000;

// Long enough for a thread left blocked in recvfrom to wake on the late datagram and run
// into the freed socket while the process is still here to see it.
constexpr std::chrono::milliseconds kSettle( 200 );

/// Swallows every send to one address and leaves the rest to the socket.
class DropSendsTo : public SocketLayerOverride
{
public:
    explicit DropSendsTo( const SystemAddress& dropped )
    : m_dropped( dropped )
    {
    }

    int RakNetSendTo( const char*, int length, const SystemAddress& systemAddress ) override
    {
        if( systemAddress == m_dropped )
            return length;
        return -1;
    }

    int RakNetRecvFrom( char[MAXIMUM_MTU_SIZE], SystemAddress*, bool ) override { return -1; }

    bool IsOverrideAddress( const SystemAddress& ) const override { return false; }

private:
    const SystemAddress m_dropped;
};

/// Runs one update cycle under RAKPEER_USER_THREADED.
void Pump( RakPeerInterface* peer )
{
#if RAKPEER_USER_THREADED == 1
    BitStream updateBitStream( MAXIMUM_MTU_SIZE );
    static_cast<RakPeer*>( peer )->RunUpdateCycle( updateBitStream );
#else
    (void)peer;
#endif
}

/// Pings target from pinger until the pong arrives. False at the deadline. Its own loop
/// rather than ConnectionWaits::WaitForMessage, because both Peers are pumped every poll.
bool AnswersPing( RakPeerInterface* pinger, RakPeerInterface* target, unsigned short targetPort )
{
    if( pinger->Ping( "127.0.0.1", targetPort, false ) == false )
        return false;
    const TimeMS deadline = GetTimeMS() + kWaitBudgetMs;
    while( ConnectionWaits::Expired( deadline ) == false )
    {
        Pump( target );
        Pump( pinger );
        if( Packet* pong = ConnectionWaits::TakeMessage( pinger, ID_UNCONNECTED_PONG ) )
        {
            pinger->DeallocatePacket( pong );
            return true;
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
    }
    return false;
}

} // namespace

TEST_CASE( "Shutdown waits for the receive thread when its wake-up datagram is lost", "[network]" )
{
    const SystemAddress peerAddress( "127.0.0.1", kPeerPort );
    // Outlives the Peer, which calls it until Shutdown returns.
    DropSendsTo dropOwnAddress( peerAddress );

    PeerScope peers;
    RakPeerInterface* peer = peers.Create();
    SocketDescriptor sd( kPeerPort, "127.0.0.1" );
    REQUIRE( peer->Startup( 1, &sd, 1 ) == RAKNET_STARTED );
    REQUIRE( AnswersPing( peers.Client( kPingerPort ), peer, kPeerPort ) );

    RakNetSocket2* socket = peer->GetSocket( UNASSIGNED_SYSTEM_ADDRESS );
    REQUIRE( socket != 0 );
    REQUIRE( socket->IsBerkleySocket() );
    static_cast<RNS2_Berkley*>( socket )->SetSocketLayerOverride( &dropOwnAddress );

    peer->Shutdown( 0 );

    RawSystemHarness::RawSystem sender( peerAddress, 0 );
    BitStream datagram;
    datagram.Write( (uint32_t)0 );
    sender.Send( datagram );
    std::this_thread::sleep_for( kSettle );
}
