#include "PlatformRandom.h"
#include "RakPeer.h"
#include "RakPeerInterface.h"
#include "RakNetTypes.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <unordered_set>
#include <vector>

/*
Guards RakNetGUID generation, which until this file existed harvested its entropy
from sixteen 1 ms sleeps and cost ~240 ms per RakPeerInterface::GetInstance() -
about half the suite's wall clock. See CONTEXT.md for what a RakNetGUID promises
and docs/adr/0001-identifiers-draw-from-the-platform-csprng.md for why the entropy
now comes from the operating system.

Deliberately tagged [guid] and NOT [network]: only the last two cases create peers,
none of them binds a socket, and `ctest -L guid` is meant to stay the fast check
while working on the generator. Nothing here may become [slow].

The cases test three different things and are kept apart on purpose:

    1. the entropy source           RakNet::FillRandomBytes
    2. the wiring around it         RakPeer::DrawGuidValue, Get64BitUniqueRandomNumber,
                                    GetInstance
    3. the cost                     GetInstance, against a loose ceiling
*/

using namespace RakNet;

namespace
{

// Fake entropy sources for the reserved-value cases below. The real CSPRNG reaches
// neither the redraw nor the give-up path - a source that returns 0, returns
// (uint64_t)-1, or fails outright is what those paths are for, and nothing but a
// substituted source can produce one. Plain functions rather than lambdas because
// DrawGuidValue takes a function pointer; hence the file-scope call counter.
int fillCallCount = 0;
uint64_t fillConstant = 0;

bool FailingSource( void*, size_t )
{
    fillCallCount++;
    return false;
}

bool ConstantSource( void* buffer, size_t bytes )
{
    fillCallCount++;
    memcpy( buffer, &fillConstant, bytes );
    return true;
}

// Both reserved values, then a usable one, so the redraw is exercised in both
// directions before it succeeds.
bool ReservedTwiceThenUsableSource( void* buffer, size_t bytes )
{
    static const uint64_t sequence[3] = { 0, UINT64_C( 0xFFFFFFFFFFFFFFFF ), UINT64_C( 0x0123456789ABCDEF ) };

    const uint64_t value = sequence[fillCallCount < 3 ? fillCallCount : 2];
    fillCallCount++;
    memcpy( buffer, &value, bytes );
    return true;
}

} // namespace

TEST_CASE( "FillRandomBytes does not repeat itself", "[guid]" )
{
    // One call for the whole buffer rather than a million small ones. Every draw is a
    // syscall now - on POSIX an open/read/close triple - so looping would spend seconds
    // on a statistical property that a single 8 MB read establishes just as well.
    const size_t wordCount = 1000000;

    std::vector<uint64_t> words( wordCount, 0 );
    REQUIRE( FillRandomBytes( words.data(), wordCount * sizeof( uint64_t ) ) );

    const std::unordered_set<uint64_t> unique( words.begin(), words.end() );
    REQUIRE( unique.size() == wordCount );
}

TEST_CASE( "FillRandomBytes handles the edges", "[guid]" )
{
    unsigned char byte = 0xAB;

    // Zero bytes is a successful no-op, and must not touch the buffer.
    REQUIRE( FillRandomBytes( &byte, 0 ) );
    REQUIRE( byte == 0xAB );

    // A null buffer is a caller error, reported rather than dereferenced.
    REQUIRE_FALSE( FillRandomBytes( 0, 8 ) );
}

TEST_CASE( "Generated GUIDs are distinct and never reserved", "[guid]" )
{
    // Both reserved values matter. 0 is Startup's "generation failed" sentinel, and
    // UNASSIGNED_RAKNET_GUID is (uint64_t)-1 - a live peer landing on it would compare
    // equal to "no peer" in AddressOrGUID::operator==.
    const size_t drawCount = 10000;

    std::unordered_set<uint64_t> seen;
    seen.reserve( drawCount );

    for( size_t i = 0; i < drawCount; i++ )
    {
        const uint64_t g = RakPeerInterface::Get64BitUniqueRandomNumber();

        REQUIRE( g != 0 );
        REQUIRE( g != UNASSIGNED_RAKNET_GUID.g );
        REQUIRE( seen.insert( g ).second );
    }
}

TEST_CASE( "DrawGuidValue never returns a reserved value", "[guid]" )
{
    // These are the paths a working entropy source never takes, which is exactly why
    // they need a substituted one: with the real CSPRNG behind it the loop always
    // returns on its first pass, and every assertion below would hold with the
    // rejection deleted.
    //
    // An unbounded loop shows up here as a hang rather than a failure - ctest's
    // TIMEOUT is the backstop - so each case also asserts how often it drew.
    fillCallCount = 0;

    SECTION( "a failing source is not retried" )
    {
        // The give-up path. Retrying a source that reports failure is the loop that
        // would never end, because 0 is both "redraw" and "the source is broken".
        REQUIRE( RakPeer::DrawGuidValue( &FailingSource ) == 0 );
        REQUIRE( fillCallCount == 1 );
    }

    SECTION( "a source stuck on 0 gives up" )
    {
        fillConstant = 0;

        REQUIRE( RakPeer::DrawGuidValue( &ConstantSource ) == 0 );
        REQUIRE( fillCallCount > 1 );
        REQUIRE( fillCallCount <= 16 );
    }

    SECTION( "a source stuck on UNASSIGNED_RAKNET_GUID gives up" )
    {
        fillConstant = UNASSIGNED_RAKNET_GUID.g;

        REQUIRE( RakPeer::DrawGuidValue( &ConstantSource ) == 0 );
        REQUIRE( fillCallCount > 1 );
        REQUIRE( fillCallCount <= 16 );
    }

    SECTION( "a reserved draw is redrawn, not returned" )
    {
        REQUIRE( RakPeer::DrawGuidValue( &ReservedTwiceThenUsableSource ) == UINT64_C( 0x0123456789ABCDEF ) );
        REQUIRE( fillCallCount == 3 );
    }

    SECTION( "the real source needs no redraw" )
    {
        const uint64_t g = RakPeer::DrawGuidValue( &FillRandomBytes );

        REQUIRE( g != 0 );
        REQUIRE( g != UNASSIGNED_RAKNET_GUID.g );
    }
}

TEST_CASE( "Peers get distinct GUIDs", "[guid]" )
{
    // The wiring test: GenerateGUID owns the reserved-value rejection, so it is worth
    // going through a real peer rather than only through the generator. Deliberately
    // fewer peers than the draw count above - this one allocates.
    const size_t peerCount = 256;

    std::vector<RakPeerInterface*> peers;
    peers.reserve( peerCount );
    std::unordered_set<uint64_t> seen;
    seen.reserve( peerCount );

    for( size_t i = 0; i < peerCount; i++ )
    {
        RakPeerInterface* peer = RakPeerInterface::GetInstance();
        REQUIRE( peer != 0 );
        peers.push_back( peer );

        // Startup is never called: a GUID exists from construction, and binding 256
        // sockets is exactly what this file is meant not to do.
        const RakNetGUID guid = peer->GetMyGUID();

        REQUIRE( guid.g != 0 );
        REQUIRE( guid != UNASSIGNED_RAKNET_GUID );
        REQUIRE( seen.insert( guid.g ).second );
    }

    for( RakPeerInterface* peer : peers )
        RakPeerInterface::DestroyInstance( peer );
}

TEST_CASE( "GetInstance does not sleep", "[guid]" )
{
    // The ceiling is deliberately loose - roughly 30x over what this should cost and
    // 30x under the ~61 s the old sleep-based generator took for the same 256
    // constructions. A tight sub-millisecond assertion is the acceptance measurement,
    // taken once by hand; as a CI assertion it would fail for reasons unrelated to
    // what it guards.
    const size_t peerCount = 256;

    std::vector<RakPeerInterface*> peers;
    peers.reserve( peerCount );

    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    for( size_t i = 0; i < peerCount; i++ )
        peers.push_back( RakPeerInterface::GetInstance() );
    const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();

    for( RakPeerInterface* peer : peers )
        RakPeerInterface::DestroyInstance( peer );

    const long long elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>( end - start ).count();
    INFO( "256 GetInstance() calls took " << elapsedMs << " ms" );
    REQUIRE( elapsedMs < 2000 );
}
