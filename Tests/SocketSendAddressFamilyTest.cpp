#include "RakNetDefines.h"
#include "RakNetSocket2.h"
#include "RakNetTypes.h"
#include "WSAStartupSingleton.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstring>
#include <functional>
#include <future>
#include <memory>
#include <thread>

/*
Drives RNS2_Berkley::Send with addresses whose family the build cannot send to.

The defect: Send_NoVDP wrapped its body in `do { ... } while( len == 0 )`, and the
branch taken by every non-AF_INET address contained only `#if RAKNET_SUPPORT_IPV6 == 1`
code. That define is 0 by default, so in the default build the branch was empty, len
stayed 0, and the loop never ended - re-running the TTL getsockopt__/setsockopt__ pair
on every pass, so a syscall storm at 100% CPU rather than a quiet spin. The loop was
backwards besides: sendto reports failure with a negative return, which the loop
*exited* on, and never with 0, which is what it retried.

No socket is opened. A default-constructed RNS2_Berkley holds INVALID_SOCKET, which is
all these need: the unsupported-family cases are rejected before the socket is touched,
and the AF_INET case wants a sendto that fails, which is exactly what an invalid handle
gives. Send is public and delegates straight to Send_NoVDP when no SocketLayerOverride
is set, so the private helper is reached without widening the header.

Every case runs under CompletesWithin. Against the unfixed code the first two hang, and
that has to surface as a deadline rather than as a wrong return value, or the only
signal would be ctest's 300 s TIMEOUT killing the whole binary.
*/

using namespace RakNet;

namespace {

// Generous by design. A correct Send_NoVDP returns in microseconds - it is one
// rejected branch or one failing syscall - so anything near this bound is the hang,
// not a slow machine.
constexpr std::chrono::milliseconds kSendDeadline( 5000 );

// Nothing here opens a socket, but sendto on Windows is documented as requiring
// WSAStartup, and the AF_INET case does reach it. Other tests get that for free from
// RakPeer::Startup; these have no peer, so they take the same refcount RakNet itself
// uses. A no-op off Windows.
struct WinsockFixture
{
    WinsockFixture() { WSAStartupSingleton::AddRef(); }
    ~WinsockFixture() { WSAStartupSingleton::Deref(); }
};

// Runs body on its own thread and waits at most timeout for it to finish. Returns false
// if it was still running at the deadline.
//
// The thread is detached rather than joined, which is deliberate and is the only way
// this can work: the failure being tested for is an unbounded loop, and a thread stuck
// in one cannot be asked to stop. Detaching lets the test report the deadline and move
// on. It leaks a spinning thread for the rest of the run, but only in a build that is
// already broken in exactly the way this file exists to catch.
bool CompletesWithin( std::chrono::milliseconds timeout, std::function<void()> body )
{
    auto task = std::make_shared<std::packaged_task<void()>>( std::move( body ) );
    std::future<void> finished = task->get_future();
    std::thread( [task]() { ( *task )(); } ).detach();
    return finished.wait_for( timeout ) == std::future_status::ready;
}

// Everything one Send call touches, in one heap block. The detached thread outlives the
// TEST_CASE body when the deadline passes, so none of this may live on the test's stack.
struct SendCase
{
    RNS2_Berkley berkleySocket;
    char payload[8];
    RNS2_SendParameters parameters;
    int result;

    explicit SendCase( int addressFamily )
    {
        memset( payload, 'x', sizeof( payload ) );

        parameters.data = payload;
        parameters.length = (int)sizeof( payload );
        parameters.ttl = 0;
        parameters.systemAddress = UNASSIGNED_SYSTEM_ADDRESS;

        // The family is the whole input under test. Port and address are left as
        // UNASSIGNED_SYSTEM_ADDRESS leaves them: nothing reads them on the paths that
        // reject the family, and the AF_INET path only needs the send to fail.
        parameters.systemAddress.address.addr4.sin_family = (decltype( parameters.systemAddress.address.addr4.sin_family ))addressFamily;

        result = 0;
    }
};

// Returns the SendCase on completion, or an empty pointer if Send was still running at
// the deadline. The pointer is shared so the detached thread keeps the block alive in
// the second case.
std::shared_ptr<SendCase> SendWithinDeadline( int addressFamily )
{
    auto sendCase = std::make_shared<SendCase>( addressFamily );

    const bool completed = CompletesWithin( kSendDeadline, [sendCase]() {
        sendCase->result = sendCase->berkleySocket.Send( &sendCase->parameters, _FILE_AND_LINE_ );
    } );

    return completed ? sendCase : std::shared_ptr<SendCase>();
}

} // namespace

TEST_CASE( "Sending to an address of no family returns instead of spinning", "[socket]" )
{
    WinsockFixture winsock;

    // AF_UNSPEC is unsendable in every configuration, so this case holds whether or not
    // RAKNET_SUPPORT_IPV6 is set.
    std::shared_ptr<SendCase> sendCase = SendWithinDeadline( AF_UNSPEC );

    REQUIRE( sendCase );
    CHECK( sendCase->result < 0 );
}

TEST_CASE( "Sending to an IPv6 address returns instead of spinning", "[socket]" )
{
    WinsockFixture winsock;

    // Deliberately not guarded on RAKNET_SUPPORT_IPV6, because both configurations owe
    // the same answer for opposite reasons and both are worth pinning. At 0 this is the
    // exact case the empty `#if RAKNET_SUPPORT_IPV6 == 1` branch produced - a family the
    // code has a branch for, in the build where that branch compiles away - and it is
    // rejected. At 1 the family is supported, so it reaches sendto__ on an invalid
    // socket, which is the only case that covers the sockaddr_in6 arm of the dispatch.
    // Either way the call returns and reports failure.
    std::shared_ptr<SendCase> sendCase = SendWithinDeadline( AF_INET6 );

    REQUIRE( sendCase );
    CHECK( sendCase->result < 0 );
}

TEST_CASE( "A failing send to an AF_INET address returns the failure once", "[socket]" )
{
    WinsockFixture winsock;

    // The other half of the backwards loop. sendto on an invalid socket fails, and a
    // failure must leave Send, not be swallowed or retried.
    std::shared_ptr<SendCase> sendCase = SendWithinDeadline( AF_INET );

    REQUIRE( sendCase );
    CHECK( sendCase->result < 0 );
}
