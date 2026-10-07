#include "PeerScope.h"
#include "RawSystem.h"

#include "BitStream.h"
#include "GetTime.h"
#include "MessageIdentifiers.h"
#include "RakNetSocket2.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

/*
Pins when a Peer's two application callbacks stop being called (ADR-0008).

Upstream stored each callback as plain fields. A setter returned at once, while the old
callback could still be running on the network or receive thread, and could even be called
again afterwards. An application that cleared a callback and then freed what it used freed
it under a running call.

The shapes pinned here:

- With the callback blocked, a setter called on another thread doesn't return until the
  callback has exited, and the callback isn't called again afterwards.
- A callback that replaces or clears itself, from inside its own call, doesn't deadlock,
  and the change applies from the next call.

RakPeerInterface functions explicitly tested:

    SetUserUpdateThread
    SetIncomingDatagramEventHandler
*/

using namespace RakNet;
using RawSystemHarness::RawSystem;

namespace {

// Hang guard for every wait below, not a settle time: each normally ends within a few
// hundred milliseconds.
constexpr TimeMS kWaitBudgetMs = 10000;

// How long a setter that doesn't wait gets to return before the callback is let go. A
// setter that does wait can't return in it, so a slow machine can only make a broken
// setter look correct, never the reverse.
constexpr int kSetterGraceMs = 200;

/// Polls \a condition until it holds or the budget is spent.
template<class Condition>
bool WaitFor( Condition condition, TimeMS budgetMs = kWaitBudgetMs )
{
    const TimeMS deadline = GetTimeMS() + budgetMs;
    while( GetTimeMS() < deadline )
    {
        if( condition() )
            return true;
        std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
    }
    return condition();
}

/// A callback body that blocks its first call until released, and counts every call.
class HeldCallback
{
public:
    void Run()
    {
        ++m_calls;
        {
            std::unique_lock<std::mutex> lock( m_mutex );
            m_entered = true;
            m_changed.notify_all();
            m_changed.wait( lock, [this] { return m_released; } );
        }
        m_exited = true;
    }

    bool WaitUntilEntered()
    {
        std::unique_lock<std::mutex> lock( m_mutex );
        return m_changed.wait_for( lock, std::chrono::milliseconds( kWaitBudgetMs ), [this] { return m_entered; } );
    }

    void Release()
    {
        std::lock_guard<std::mutex> guard( m_mutex );
        m_released = true;
        m_changed.notify_all();
    }

    bool Exited() const { return m_exited.load(); }
    int Calls() const { return m_calls.load(); }

private:
    std::mutex m_mutex;
    std::condition_variable m_changed;
    bool m_entered = false;
    bool m_released = false;
    std::atomic<bool> m_exited{ false };
    std::atomic<int> m_calls{ 0 };
};

struct ClearOutcome
{
    bool callbackExitedFirst = false;
    int callsAtReturn = 0;
};

/// With \a held blocked in its callback, calls \a clear on another thread and reports
/// whether the callback had exited by the time \a clear returned. Releases \a held either
/// way, so the Peer can shut down.
template<class Clear>
ClearOutcome ClearWhileHeld( HeldCallback& held, Clear clear )
{
    ClearOutcome outcome;
    REQUIRE( held.WaitUntilEntered() );

    std::atomic<bool> returned{ false };
    std::thread clearer( [&] {
        clear();
        outcome.callbackExitedFirst = held.Exited();
        outcome.callsAtReturn = held.Calls();
        returned = true;
    } );

    WaitFor( [&] { return returned.load(); }, kSetterGraceMs );
    held.Release();
    clearer.join();
    return outcome;
}

SystemAddress LoopbackAddressOf( RakPeerInterface* peer )
{
    SystemAddress address;
    address.FromStringExplicitPort( "127.0.0.1", peer->GetMyBoundAddress().GetPort() );
    return address;
}

/// Two bytes that are no RakNet message, so the Peer discards them once the handler lets
/// them through.
void SendJunkDatagram( RawSystem& sender )
{
    BitStream junk;
    junk.Write( (MessageID)ID_USER_PACKET_ENUM );
    junk.Write( (MessageID)0 );
    sender.Send( junk );
}

/// An ID_UNCONNECTED_PING, field for field as RakPeer::Ping writes it. The Peer passes it
/// to Receive, so a test can count which datagrams got through.
void SendUnconnectedPing( RawSystem& sender )
{
    BitStream ping;
    ping.Write( (MessageID)ID_UNCONNECTED_PING );
    ping.Write( RakNet::GetTime() );
    ping.WriteAlignedBytes( RawSystemHarness::OFFLINE_MESSAGE_DATA_ID, sizeof( RawSystemHarness::OFFLINE_MESSAGE_DATA_ID ) );
    ping.Write( RakNetGUID( 0x5eed ) );
    sender.Send( ping );
}

void HeldUpdateCallback( RakPeerInterface*, void* data )
{
    static_cast<HeldCallback*>( data )->Run();
}

HeldCallback* s_heldHandler = nullptr;

bool HeldDatagramHandler( RNS2RecvStruct* )
{
    s_heldHandler->Run();
    return true;
}

/// Replaces itself with Second on its first call.
struct SelfReplacing
{
    RakPeerInterface* peer = nullptr;
    std::atomic<int> firstCalls{ 0 };
    std::atomic<int> secondCalls{ 0 };

    static void First( RakPeerInterface*, void* data )
    {
        SelfReplacing* self = static_cast<SelfReplacing*>( data );
        ++self->firstCalls;
        self->peer->SetUserUpdateThread( &SelfReplacing::Second, self );
    }

    static void Second( RakPeerInterface*, void* data )
    {
        SelfReplacing* self = static_cast<SelfReplacing*>( data );
        if( self->secondCalls++ == 0 )
            self->peer->SetUserUpdateThread( nullptr, nullptr );
    }
};

RakPeerInterface* s_selfClearingPeer = nullptr;
std::atomic<int> s_selfClearingCalls{ 0 };

bool SelfClearingDatagramHandler( RNS2RecvStruct* )
{
    ++s_selfClearingCalls;
    s_selfClearingPeer->SetIncomingDatagramEventHandler( nullptr );
    return true;
}

} // namespace

TEST_CASE( "SetUserUpdateThread returns only once the callback it replaces has exited", "[network]" )
{
    PeerScope peers;
    RakPeerInterface* peer = peers.Client();

    HeldCallback held;
    peer->SetUserUpdateThread( &HeldUpdateCallback, &held );

    const ClearOutcome outcome = ClearWhileHeld( held, [&] { peer->SetUserUpdateThread( nullptr, nullptr ); } );
    CHECK( outcome.callbackExitedFirst );

    // Several network-thread passes, each of which would call a callback still installed.
    std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
    CHECK( held.Calls() == outcome.callsAtReturn );
}

TEST_CASE( "SetIncomingDatagramEventHandler returns only once the handler it replaces has exited", "[network]" )
{
    PeerScope peers;
    RakPeerInterface* peer = peers.Client();
    RawSystem sender( LoopbackAddressOf( peer ), 0x61 );

    HeldCallback held;
    s_heldHandler = &held;
    peer->SetIncomingDatagramEventHandler( &HeldDatagramHandler );
    SendJunkDatagram( sender );

    const ClearOutcome outcome = ClearWhileHeld( held, [&] { peer->SetIncomingDatagramEventHandler( nullptr ); } );
    CHECK( outcome.callbackExitedFirst );

    SendJunkDatagram( sender );
    std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
    CHECK( held.Calls() == outcome.callsAtReturn );
    s_heldHandler = nullptr;
}

TEST_CASE( "A user-update callback can replace and then clear itself from inside its own call", "[network]" )
{
    PeerScope peers;
    RakPeerInterface* peer = peers.Client();

    SelfReplacing callbacks;
    callbacks.peer = peer;
    peer->SetUserUpdateThread( &SelfReplacing::First, &callbacks );

    REQUIRE( WaitFor( [&] { return callbacks.secondCalls.load() > 0; } ) );

    // Several network-thread passes, each of which would call a callback still installed.
    std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
    CHECK( callbacks.firstCalls == 1 );
    CHECK( callbacks.secondCalls == 1 );
}

TEST_CASE( "A datagram handler can clear itself from inside its own call", "[network]" )
{
    PeerScope peers;
    RakPeerInterface* peer = peers.Client();
    RawSystem sender( LoopbackAddressOf( peer ), 0x62 );

    s_selfClearingPeer = peer;
    s_selfClearingCalls = 0;
    peer->SetIncomingDatagramEventHandler( &SelfClearingDatagramHandler );

    SendJunkDatagram( sender );
    REQUIRE( WaitFor( [&] { return s_selfClearingCalls.load() > 0; } ) );

    // The receive thread is still running: later datagrams reach the Peer, past no handler.
    constexpr unsigned int kPings = 5;
    for( unsigned int i = 0; i < kPings; ++i )
        SendUnconnectedPing( sender );
    CHECK( WaitFor( [&] { return peer->GetReceiveBufferSize() == kPings; } ) );
    CHECK( s_selfClearingCalls == 1 );
    s_selfClearingPeer = nullptr;
}
