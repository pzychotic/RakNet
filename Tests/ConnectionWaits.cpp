#include "ConnectionWaits.h"

#include "CommonFunctions.h"
#include "RakNetStringMakers.h"

#include "GetTime.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <sstream>
#include <thread>
#include <vector>

using namespace RakNet;

namespace {

// The state goes into a FAIL message composed here rather than into a comparison
// inside a REQUIRE, and a stream insertion does not go through Catch2's
// stringification, so the registered names have to be asked for by name. Same
// table either way - see RakNetStringMakers.h.
std::string ToString( ConnectionState state )
{
    return Catch::StringMaker<ConnectionState>::convert( state );
}

} // namespace

bool ConnectionWaits::Expired( TimeMS deadline )
{
    return static_cast<int32_t>( GetTimeMS() - deadline ) >= 0;
}

void ConnectionWaits::WaitForRequestToSettle( RakPeerInterface* peer, SystemAddress addr, TimeMS deadline )
{
    // isConnected = false, isConnecting = true, isPending = true: keep looping only
    // while the request is still in flight. See the header - IS_CONNECTED and
    // IS_NOT_CONNECTED both end the wait.
    while( CommonFunctions::ConnectionStateMatchesOptions( peer, addr, false, true, true ) )
    {
        if( Expired( deadline ) )
        {
            // FAIL rather than a REQUIRE on the predicate, which would print a
            // naked `false`. The state is the whole diagnosis.
            FAIL( "connection request never settled: stuck in " << ToString( peer->GetConnectionState( addr ) ) );
        }

        std::this_thread::sleep_for( std::chrono::milliseconds( kPollInterval ) );
    }
}

void ConnectionWaits::WaitForRequestsToSettle( RakPeerInterface* const* peers, int count, SystemAddress addr )
{
    const TimeMS deadline = GetTimeMS() + kSettleBudget;

    for( int i = 0; i < count; i++ )
    {
        // Scoped to the enclosing test case, so the FAIL inside the primitive says
        // which peer it was without the primitive having to take an index.
        INFO( "peer " << i );
        WaitForRequestToSettle( peers[i], addr, deadline );
    }
}

void ConnectionWaits::WaitForAllPairsToSettle( RakPeerInterface* const* peers, int count, unsigned short basePort )
{
    const TimeMS deadline = GetTimeMS() + kSettleBudget;

    for( int i = 0; i < count; i++ )
    {
        for( int j = 0; j < count; j++ )
        {
            if( i == j )
            {
                continue;
            }

            const SystemAddress target( "127.0.0.1", static_cast<unsigned short>( basePort + j ) );

            // Scoped to this pair, so the FAIL inside the primitive names both
            // ends of it without the primitive having to take two indices.
            INFO( "peer " << i << " toward peer " << j << " on port " << target.GetPort() );
            WaitForRequestToSettle( peers[i], target, deadline );
        }
    }
}

void ConnectionWaits::WaitForConnectionCounts( RakPeerInterface* const* peers, int count, int expectedCount )
{
    const TimeMS deadline = GetTimeMS() + kConnectionCountBudget;

    std::vector<SystemAddress> systemList;
    std::vector<RakNetGUID> guidList;
    std::vector<int> actualCounts( static_cast<size_t>( count ) );

    for( ;; )
    {
        bool allMatch = true;

        // Every peer read every pass, even once one of them has already
        // mismatched: the counts are what the failure message below is made of,
        // and a short-circuit here would leave it with holes in it.
        for( int i = 0; i < count; i++ )
        {
            peers[i]->GetSystemList( systemList, guidList );
            actualCounts[i] = static_cast<int>( guidList.size() );

            if( actualCounts[i] != expectedCount )
            {
                allMatch = false;
            }
        }

        if( allMatch )
        {
            return;
        }

        if( Expired( deadline ) )
        {
            std::ostringstream report;

            for( int i = 0; i < count; i++ )
            {
                report << "\n  peer " << i << ": " << actualCounts[i];
            }

            // FAIL rather than a REQUIRE per peer: this is one verdict about the
            // whole set, and the point of the message is that it names every peer
            // rather than stopping at the first one that is short.
            FAIL( "peers never all reached " << expectedCount
                                             << " connections. Actual connection counts at expiry:" << report.str() );
        }

        std::this_thread::sleep_for( std::chrono::milliseconds( kPollInterval ) );
    }
}

void ConnectionWaits::WaitForAttemptsToBeCancelled( RakPeerInterface* const* peers, int count, SystemAddress addr )
{
    const TimeMS deadline = GetTimeMS() + kCancelBudget;

    for( int i = 0; i < count; i++ )
    {
        while( peers[i]->GetConnectionState( addr ) == IS_PENDING )
        {
            if( Expired( deadline ) )
            {
                // FAIL rather than leaving expiry to fall through into the
                // caller's assertion on the resulting state, which would report a
                // budget that ran out identically to an attempt that went
                // somewhere unexpected. Still queued is the diagnosis, and only
                // this function is in a position to make it.
                FAIL( "peer " << i << ": connection attempt toward port " << addr.GetPort()
                              << " was never cancelled - still IS_PENDING after " << kCancelBudget << " ms" );
            }

            std::this_thread::sleep_for( std::chrono::milliseconds( kPollInterval ) );
        }
    }
}

void ConnectionWaits::WaitForDisconnect( RakPeerInterface* peer, SystemAddress addr )
{
    const TimeMS deadline = GetTimeMS() + kDisconnectBudget;

    // isConnected = isConnecting = isPending = isDisconnecting = true: keep looping
    // until the connection is gone by any route. The CloseConnection is the call
    // site's, issued once - see the header for why it must not be in this body.
    while( CommonFunctions::ConnectionStateMatchesOptions( peer, addr, true, true, true, true ) )
    {
        if( Expired( deadline ) )
        {
            // FAIL rather than a REQUIRE on the predicate, as above. The state is
            // the diagnosis: IS_DISCONNECTING here means the close was applied and
            // the connection would not finish leaving, which is a different fault
            // from IS_CONNECTED, where it was never applied at all.
            FAIL( "connection toward port " << addr.GetPort() << " never closed - still "
                                            << ToString( peer->GetConnectionState( addr ) ) << " after "
                                            << kDisconnectBudget << " ms" );
        }

        std::this_thread::sleep_for( std::chrono::milliseconds( kPollInterval ) );
    }
}

void ConnectionWaits::Drain( RakPeerInterface* peer )
{
    for( Packet* packet = peer->Receive(); packet; peer->DeallocatePacket( packet ), packet = peer->Receive() )
    {
    }
}

void ConnectionWaits::DrainAll( RakPeerInterface* const* peers, int count )
{
    for( int i = 0; i < count; i++ )
    {
        Drain( peers[i] );
    }
}
