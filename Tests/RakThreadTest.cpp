#include "RakThread.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>

#if defined( _WIN32 )
#include "WindowsIncludes.h"
#else
#include <pthread.h>
#include <sched.h>
#endif

/*
Guards RakThread::Create, which starts every thread the library owns.

It creates threads natively rather than through std::thread, whose constructor reports
failure only by throwing (ADR-0004). What these cases pin is what the callers depend on:
the function runs with its argument on a new thread, a capturing callable survives the
trip, and the priority is in force from the thread's first instruction rather than set
some time after it started.

A case that makes creation fail is not here. Neither _beginthreadex nor pthread_create
can be made to fail on demand without exhausting the machine's threads or address space,
and a stack size large enough to be refused on every platform does not exist: a 64-bit
Windows process reserves 4 GiB without complaint. A test-only failure hook in Source/
would test the hook, not the OS path.

Tagged [thread]: nothing here binds a socket, and each case waits well under a second.
*/

using namespace RakNet;

namespace {

const int unreadPriority = -12345;

int CurrentThreadPriority()
{
#if defined( _WIN32 )
    return GetThreadPriority( GetCurrentThread() );
#else
    int policy;
    sched_param param;
    if( pthread_getschedparam( pthread_self(), &policy, &param ) != 0 )
        return unreadPriority;
    return param.sched_priority;
#endif
}

/// Filled in by the new thread, waited on by the test.
struct Observation
{
    std::mutex mutex;
    std::condition_variable ran;
    bool done = false;
    void* arg = nullptr;
    int priority = unreadPriority;

    bool WaitForRun()
    {
        std::unique_lock<std::mutex> lock( mutex );
        return ran.wait_for( lock, std::chrono::seconds( 5 ), [this] { return done; } );
    }

    /// Called as the new thread's first act, so the priority it reads is the one it started with.
    void RecordFromThisThread( void* observedArg )
    {
        const int observedPriority = CurrentThreadPriority();
        std::lock_guard<std::mutex> lock( mutex );
        arg = observedArg;
        priority = observedPriority;
        done = true;
        ran.notify_one();
    }

    /// A capturing callable, the shape a free-function-only Create() would reject.
    std::function<void( void* )> Recorder()
    {
        return [this]( void* observedArg ) { RecordFromThisThread( observedArg ); };
    }
};

/// The Observation travels as the argument, the way every in-tree caller passes its `this`.
void RecordFromFreeFunction( void* observation )
{
    static_cast<Observation*>( observation )->RecordFromThisThread( observation );
}

} // namespace

TEST_CASE( "Create runs a free function with its argument", "[thread]" )
{
    Observation observation;
    REQUIRE( RakThread::Create( RecordFromFreeFunction, &observation ) == 0 );
    REQUIRE( observation.WaitForRun() );
    REQUIRE( observation.arg == &observation );
}

// The parameter is a std::function, so a lambda with captures is public API and must keep
// working once the callable crosses into a native thread entry point.
TEST_CASE( "Create runs a capturing lambda", "[thread]" )
{
    Observation observation;
    int marker = 0;
    REQUIRE( RakThread::Create( observation.Recorder(), &marker ) == 0 );
    REQUIRE( observation.WaitForRun() );
    REQUIRE( observation.arg == &marker );
}

#if defined( _WIN32 )
// The priority is read as the thread's first act. Set after the thread starts, as
// std::thread forced, this read races the creator's SetThreadPriority.
TEST_CASE( "Create applies the priority before the thread runs", "[thread]" )
{
    for( int priority : { THREAD_PRIORITY_BELOW_NORMAL, THREAD_PRIORITY_ABOVE_NORMAL, THREAD_PRIORITY_LOWEST } )
    {
        Observation observation;
        REQUIRE( RakThread::Create( observation.Recorder(), nullptr, priority ) == 0 );
        REQUIRE( observation.WaitForRun() );
        REQUIRE( observation.priority == priority );
    }
}
#else
// RakPeer and TCPInterface pass 1000 when the caller leaves the priority at its default,
// which is outside every POSIX policy's range. Creation must still succeed, on the
// creator's own scheduling.
TEST_CASE( "Create ignores a priority outside the policy's range", "[thread]" )
{
    Observation observation;
    REQUIRE( RakThread::Create( observation.Recorder(), nullptr, 1000 ) == 0 );
    REQUIRE( observation.WaitForRun() );
    REQUIRE( observation.priority == CurrentThreadPriority() );
}

// A smoke test only. An unprivileged process runs under SCHED_OTHER, whose range is a
// single value on Linux, so the priority asked for is the one the thread would inherit
// anyway and this passes whether or not it was applied. A priority that actually differs
// needs SCHED_FIFO or SCHED_RR, which needs privileges a test run does not have.
TEST_CASE( "Create accepts the policy's minimum priority", "[thread]" )
{
    int policy;
    sched_param param;
    REQUIRE( pthread_getschedparam( pthread_self(), &policy, &param ) == 0 );
    const int priority = sched_get_priority_min( policy );

    Observation observation;
    REQUIRE( RakThread::Create( observation.Recorder(), nullptr, priority ) == 0 );
    REQUIRE( observation.WaitForRun() );
}
#endif
