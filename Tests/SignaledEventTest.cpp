#include "SignaledEvent.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

/*
Pins SignaledEvent, which wakes RakPeer's network thread when there is work for it.

The event is a binary latch with one waiter. A signal before the wait is kept, a signal
during the wait ends it, and a wait with no signal times out. Signals coalesce: two of them
before a wait end that one wait, not two.

Each "returns at once" case waits 5 s, so a lost signal fails on the upper bound instead
of passing slowly. Tagged [thread]: nothing here binds a socket.
*/

using namespace RakNet;

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kLongWaitMs = 5000;
constexpr int kShortWaitMs = 50;

// Far below kLongWaitMs and far above any scheduling delay.
constexpr std::chrono::milliseconds kEarlyReturn( 1000 );

// Long enough that the wait has begun, well below kEarlyReturn.
constexpr std::chrono::milliseconds kSignalDelay( 100 );

// The waitable timer and the steady clock tick independently, so allow one millisecond.
constexpr std::chrono::milliseconds kTimeoutSlack( 1 );

std::chrono::milliseconds TimedWait( SignaledEvent& event, int timeoutMs )
{
    const Clock::time_point start = Clock::now();
    event.WaitOnEvent( timeoutMs );
    return std::chrono::duration_cast<std::chrono::milliseconds>( Clock::now() - start );
}

} // namespace

TEST_CASE( "SignaledEvent wait returns at once after an earlier signal", "[thread]" )
{
    SignaledEvent event;
    event.SetEvent();
    REQUIRE( TimedWait( event, kLongWaitMs ) < kEarlyReturn );
}

TEST_CASE( "SignaledEvent signal from another thread ends a wait early", "[thread]" )
{
    SignaledEvent event;
    std::thread signaler( [&event] {
        std::this_thread::sleep_for( kSignalDelay );
        event.SetEvent();
    } );
    const std::chrono::milliseconds waited = TimedWait( event, kLongWaitMs );
    signaler.join();
    REQUIRE( waited < kEarlyReturn );
}

TEST_CASE( "SignaledEvent wait without a signal lasts the timeout", "[thread]" )
{
    SignaledEvent event;
    const std::chrono::milliseconds waited = TimedWait( event, kShortWaitMs );
    REQUIRE( waited >= std::chrono::milliseconds( kShortWaitMs ) - kTimeoutSlack );
    REQUIRE( waited < kEarlyReturn );
}

TEST_CASE( "SignaledEvent signals coalesce into one early return", "[thread]" )
{
    SignaledEvent event;
    event.SetEvent();
    event.SetEvent();
    REQUIRE( TimedWait( event, kLongWaitMs ) < kEarlyReturn );
    const std::chrono::milliseconds waited = TimedWait( event, kShortWaitMs );
    REQUIRE( waited >= std::chrono::milliseconds( kShortWaitMs ) - kTimeoutSlack );
    REQUIRE( waited < kEarlyReturn );
}
