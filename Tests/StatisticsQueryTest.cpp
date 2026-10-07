#include "BitStream.h"
#include "ConnectionWaits.h"
#include "GetTime.h"
#include "MessageIdentifiers.h"
#include "RakNetDefines.h"
#include "RakNetStatistics.h"
#include "RakNetStringMakers.h"
#include "RakPeer.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <thread>
#include <vector>

/*
Pins the statistics queries to the network thread (ADR-0007, point 4). The user thread
never reads a reliability layer: GetStatistics and GetStatisticsList post a query, and the
network thread answers it at the end of its update cycle.

The shapes pinned here:

- Over a connection that has carried a known amount of data, GetStatistics by address, by
  index and GetStatisticsList report at least that much sent, and the same values as each
  other.
- Askers on several threads each get the answer to their own question.
- A query that times out hands its answer to nobody: the next caller gets its own.
- A query asked on the network thread itself is answered there and then.
- During and after Shutdown the queries fail promptly.

Under RAKPEER_USER_THREADED the test runs every update cycle itself, and the queries are
answered inline. The cases that need a network thread are left out there.

RakPeerInterface functions explicitly tested:

    GetStatistics
    GetStatisticsList
*/

using namespace RakNet;

namespace {

// Ports no other test uses.
constexpr unsigned short kAgreeServerPort = 32100;
constexpr unsigned short kAgreeClientPort = 32101;
constexpr unsigned short kConcurrentServerPort = 32110;
constexpr unsigned short kConcurrentSmallClientPort = 32111;
constexpr unsigned short kConcurrentLargeClientPort = 32112;
constexpr unsigned short kTimeoutServerPort = 32120;
constexpr unsigned short kTimeoutClientPort = 32121;
constexpr unsigned short kInlineServerPort = 32130;
constexpr unsigned short kInlineClientPort = 32131;
constexpr unsigned short kShutdownServerPort = 32140;
constexpr unsigned short kShutdownClientPort = 32141;
constexpr unsigned short kAfterShutdownServerPort = 32150;
constexpr unsigned short kAfterShutdownClientPort = 32151;

// Hang guard for each wait below.
constexpr TimeMS kWaitBudgetMs = 10000;
// A query that fails because the peer is shut down fails without waiting for the network
// thread, so it takes nothing like BLOCKING_QUERY_TIMEOUT_MS.
constexpr TimeMS kPromptMs = BLOCKING_QUERY_TIMEOUT_MS / 2;

constexpr int kMessageBytes = 1000;

/// A Peer bound to 127.0.0.1 on a fixed port, that counts the user messages it receives.
class Peer
{
public:
    Peer( unsigned short port, unsigned int maxConnections )
    : peer( static_cast<RakPeer*>( RakPeerInterface::GetInstance() ) )
    {
        SocketDescriptor socketDescriptor( port, "127.0.0.1" );
        REQUIRE( peer->Startup( maxConnections, &socketDescriptor, 1 ) == RAKNET_STARTED );
        peer->SetMaximumIncomingConnections( (unsigned short)maxConnections );
        address.FromStringExplicitPort( "127.0.0.1", port );
    }

    ~Peer()
    {
        RakPeerInterface::DestroyInstance( peer );
    }

    Peer( const Peer& ) = delete;
    Peer& operator=( const Peer& ) = delete;

    RakPeer* operator->() const { return peer; }
    RakPeer* Get() const { return peer; }
    const SystemAddress& Address() const { return address; }
    int UserMessagesReceived() const { return userMessagesReceived; }

    /// Runs one update cycle under RAKPEER_USER_THREADED, then takes every Packet waiting.
    void Pump()
    {
#if RAKPEER_USER_THREADED == 1
        BitStream updateBitStream( MAXIMUM_MTU_SIZE );
        peer->RunUpdateCycle( updateBitStream );
#endif
        for( Packet* packet = peer->Receive(); packet != 0; packet = peer->Receive() )
        {
            if( packet->length > 0 && packet->data[0] == ID_USER_PACKET_ENUM )
                userMessagesReceived++;
            peer->DeallocatePacket( packet );
        }
    }

private:
    RakPeer* peer;
    SystemAddress address;
    int userMessagesReceived = 0;
};

bool Connect( Peer& client, Peer& server )
{
    if( client->Connect( "127.0.0.1", server.Address().GetPort(), 0, 0 ) != CONNECTION_ATTEMPT_STARTED )
        return false;
    return ConnectionWaits::WaitUntil(
        [&] {
            client.Pump();
            server.Pump();
            return client->GetConnectionState( server.Address() ) == IS_CONNECTED && server->GetConnectionState( client.Address() ) == IS_CONNECTED;
        },
        kWaitBudgetMs );
}

/// Sends \a count user messages of kMessageBytes from \a from to \a to, and waits until they
/// have all arrived and \a from holds none for resending, so its counters for them are final.
bool SendAndSettle( Peer& from, Peer& to, int count )
{
    const int receivedBefore = to.UserMessagesReceived();
    char message[kMessageBytes] = {};
    message[0] = (char)ID_USER_PACKET_ENUM;
    for( int i = 0; i < count; i++ )
        from->Send( message, kMessageBytes, HIGH_PRIORITY, RELIABLE_ORDERED, 0, to.Address(), false );

    return ConnectionWaits::WaitUntil(
        [&] {
            from.Pump();
            to.Pump();
            if( to.UserMessagesReceived() < receivedBefore + count )
                return false;
            RakNetStatistics statistics;
            return from->GetStatistics( to.Address(), &statistics ) != 0 && statistics.messagesInResendBuffer == 0;
        },
        kWaitBudgetMs );
}

/// The fields that stay put while a settled connection carries only its keepalive pings,
/// and even those only between pings.
bool SameCounters( const RakNetStatistics& a, const RakNetStatistics& b )
{
    for( int i = 0; i < RNS_PER_SECOND_METRICS_COUNT; i++ )
    {
        if( a.runningTotal[i] != b.runningTotal[i] )
            return false;
    }
    return a.connectionStartTime == b.connectionStartTime;
}

/// The entry for \a address in a GetStatisticsList answer, or null.
const RakNetStatistics* Find( const std::vector<SystemAddress>& addresses, const std::vector<RakNetStatistics>& statistics, const SystemAddress& address )
{
    for( size_t i = 0; i < addresses.size(); i++ )
    {
        if( addresses[i] == address )
            return &statistics[i];
    }
    return nullptr;
}

} // namespace

TEST_CASE( "GetStatistics and GetStatisticsList report the data sent over a connection, and agree", "[network]" )
{
    Peer server( kAgreeServerPort, 1 );
    Peer client( kAgreeClientPort, 1 );
    REQUIRE( Connect( client, server ) );

    constexpr int kMessages = 20;
    REQUIRE( SendAndSettle( client, server, kMessages ) );
    const uint64_t sent = (uint64_t)kMessages * kMessageBytes;

    const int index = client->GetIndexFromSystemAddress( server.Address() );
    REQUIRE( index >= 0 );

    // A keepalive ping between two readings moves the counters, so read the list on both
    // sides of the others and retry until nothing moved in between.
    bool agreed = false;
    for( int attempt = 0; attempt < 20 && agreed == false; attempt++ )
    {
        std::vector<SystemAddress> addresses;
        std::vector<RakNetGUID> guids;
        std::vector<RakNetStatistics> before;
        client->GetStatisticsList( addresses, guids, before );
        REQUIRE( addresses.size() == 1 );
        REQUIRE( guids.size() == 1 );
        REQUIRE( before.size() == 1 );
        CHECK( addresses[0] == server.Address() );
        CHECK( guids[0] == server->GetMyGUID() );

        RakNetStatistics byAddress;
        REQUIRE( client->GetStatistics( server.Address(), &byAddress ) == &byAddress );
        RakNetStatistics byIndex;
        REQUIRE( client->GetStatistics( (unsigned int)index, &byIndex ) );
        RakNetStatistics* staticStatistics = client->GetStatistics( server.Address() );
        REQUIRE( staticStatistics != nullptr );
        const RakNetStatistics fromStatic = *staticStatistics;
        RakNetStatistics sum;
        REQUIRE( client->GetStatistics( UNASSIGNED_SYSTEM_ADDRESS, &sum ) == &sum );

        std::vector<RakNetStatistics> after;
        client->GetStatisticsList( addresses, guids, after );
        REQUIRE( after.size() == 1 );

        CHECK( before[0].runningTotal[USER_MESSAGE_BYTES_PUSHED] >= sent );
        CHECK( before[0].runningTotal[USER_MESSAGE_BYTES_SENT] >= sent );
        CHECK( byAddress.runningTotal[USER_MESSAGE_BYTES_SENT] >= sent );

        if( SameCounters( before[0], after[0] ) == false )
        {
            server.Pump();
            client.Pump();
            continue;
        }

        CHECK( SameCounters( byAddress, before[0] ) );
        CHECK( SameCounters( byIndex, before[0] ) );
        CHECK( SameCounters( fromStatic, before[0] ) );
        CHECK( SameCounters( sum, before[0] ) );
        agreed = true;
    }
    CHECK( agreed );

    {
        INFO( "an address with no connection" );
        RakNetStatistics statistics;
        CHECK( client->GetStatistics( client.Address(), &statistics ) == nullptr );
        CHECK( client->GetStatistics( (unsigned int)client->GetMaximumNumberOfPeers(), &statistics ) == false );
    }
}

#if RAKPEER_USER_THREADED != 1

TEST_CASE( "Concurrent statistics queries each get the answer to their own question", "[network]" )
{
    Peer server( kConcurrentServerPort, 2 );
    Peer smallClient( kConcurrentSmallClientPort, 1 );
    Peer largeClient( kConcurrentLargeClientPort, 1 );
    REQUIRE( Connect( smallClient, server ) );
    REQUIRE( Connect( largeClient, server ) );

    constexpr int kSmallMessages = 2;
    constexpr int kLargeMessages = 200;
    REQUIRE( SendAndSettle( smallClient, server, kSmallMessages ) );
    REQUIRE( SendAndSettle( largeClient, server, kLargeMessages ) );

    // What the server received from each, with room for the handshake and pings.
    const uint64_t smallCeiling = (uint64_t)kLargeMessages * kMessageBytes / 2;
    const uint64_t largeFloor = (uint64_t)kLargeMessages * kMessageBytes;

    constexpr int kQueriesPerThread = 100;
    std::atomic<int> wrongAnswers{ 0 };
    std::atomic<int> failedQueries{ 0 };
    auto ask = [&]( SystemAddress address, bool large ) {
        for( int i = 0; i < kQueriesPerThread; i++ )
        {
            RakNetStatistics statistics;
            if( server->GetStatistics( address, &statistics ) == nullptr )
            {
                failedQueries++;
                continue;
            }
            const uint64_t received = statistics.runningTotal[USER_MESSAGE_BYTES_RECEIVED_PROCESSED];
            if( large ? received < largeFloor : received >= smallCeiling )
                wrongAnswers++;
        }
    };
    auto askForList = [&] {
        for( int i = 0; i < kQueriesPerThread; i++ )
        {
            std::vector<SystemAddress> addresses;
            std::vector<RakNetGUID> guids;
            std::vector<RakNetStatistics> statistics;
            server->GetStatisticsList( addresses, guids, statistics );
            const RakNetStatistics* fromSmall = Find( addresses, statistics, smallClient.Address() );
            const RakNetStatistics* fromLarge = Find( addresses, statistics, largeClient.Address() );
            if( fromSmall == nullptr || fromLarge == nullptr )
            {
                failedQueries++;
                continue;
            }
            if( fromSmall->runningTotal[USER_MESSAGE_BYTES_RECEIVED_PROCESSED] >= smallCeiling || fromLarge->runningTotal[USER_MESSAGE_BYTES_RECEIVED_PROCESSED] < largeFloor )
                wrongAnswers++;
        }
    };

    std::thread smallAsker( ask, smallClient.Address(), false );
    std::thread largeAsker( ask, largeClient.Address(), true );
    std::thread listAsker( askForList );
    smallAsker.join();
    largeAsker.join();
    listAsker.join();

    CHECK( failedQueries == 0 );
    CHECK( wrongAnswers == 0 );
}

namespace {

/// Holds up the network thread, through SetUserUpdateThread, for as long as it's told to.
struct Staller
{
    std::atomic<int> stallMs{ 0 };
    std::atomic<bool> stalling{ false };

    static void Callback( RakPeerInterface*, void* data )
    {
        Staller* self = static_cast<Staller*>( data );
        const int ms = self->stallMs.exchange( 0 );
        if( ms == 0 )
            return;
        self->stalling = true;
        std::this_thread::sleep_for( std::chrono::milliseconds( ms ) );
        self->stalling = false;
    }
};

} // namespace

TEST_CASE( "A statistics query that times out hands its answer to nobody", "[network]" )
{
    Peer server( kTimeoutServerPort, 1 );
    Peer client( kTimeoutClientPort, 1 );
    REQUIRE( Connect( client, server ) );

    Staller staller;
    server->SetUserUpdateThread( &Staller::Callback, &staller );
    staller.stallMs = BLOCKING_QUERY_TIMEOUT_MS + 250;
    REQUIRE( ConnectionWaits::WaitUntil(
        [&] {
            client.Pump();
            return staller.stalling.load();
        },
        kWaitBudgetMs ) );

    RakNetStatistics statistics;
    const TimeMS start = GetTimeMS();
    CHECK( server->GetStatistics( client.Address(), &statistics ) == nullptr );
    const TimeMS waited = GetTimeMS() - start;
    CHECK( waited >= BLOCKING_QUERY_TIMEOUT_MS - 50 );

    REQUIRE( ConnectionWaits::WaitUntil(
        [&] {
            client.Pump();
            return staller.stalling.load() == false;
        },
        kWaitBudgetMs ) );

    // The network thread answers the abandoned query too, at the end of the cycle the
    // stall held up. None of that answer may reach these.
    CHECK( server->GetStatistics( server.Address(), &statistics ) == nullptr );
    CHECK( server->GetStatistics( client.Address(), &statistics ) == &statistics );

    server->SetUserUpdateThread( nullptr, nullptr );
}

TEST_CASE( "A statistics query asked on the network thread is answered there", "[network]" )
{
    Peer server( kInlineServerPort, 1 );
    Peer client( kInlineClientPort, 1 );
    REQUIRE( Connect( client, server ) );

    struct Asker
    {
        SystemAddress target;
        std::atomic<bool> asked{ false };
        std::atomic<bool> answered{ false };
        std::atomic<TimeMS> took{ 0 };
        RakPeer* peer = nullptr;

        static void Callback( RakPeerInterface*, void* data )
        {
            Asker* self = static_cast<Asker*>( data );
            if( self->asked.exchange( true ) )
                return;
            RakNetStatistics statistics;
            const TimeMS start = GetTimeMS();
            self->answered = self->peer->GetStatistics( self->target, &statistics ) != nullptr;
            self->took = GetTimeMS() - start;
        }
    } asker;
    asker.target = client.Address();
    asker.peer = server.Get();

    server->SetUserUpdateThread( &Asker::Callback, &asker );
    REQUIRE( ConnectionWaits::WaitUntil(
        [&] {
            client.Pump();
            server.Pump();
            return asker.asked.load() && server->GetStatistics( client.Address() ) != nullptr;
        },
        kWaitBudgetMs ) );
    server->SetUserUpdateThread( nullptr, nullptr );

    CHECK( asker.answered );
    CHECK( asker.took < kPromptMs );
}

TEST_CASE( "A statistics query in flight when Shutdown starts fails promptly", "[network]" )
{
    Peer server( kShutdownServerPort, 1 );
    Peer client( kShutdownClientPort, 1 );
    REQUIRE( Connect( client, server ) );

    std::atomic<bool> stop{ false };
    std::atomic<int> answered{ 0 };
    std::atomic<TimeMS> longestFailure{ 0 };
    std::thread asker( [&] {
        while( stop == false )
        {
            RakNetStatistics statistics;
            const TimeMS start = GetTimeMS();
            if( server->GetStatistics( client.Address(), &statistics ) != nullptr )
            {
                answered++;
                continue;
            }
            const TimeMS took = GetTimeMS() - start;
            if( took > longestFailure )
                longestFailure = took;
        }
    } );
    // A failed REQUIRE unwinds through here, and a joinable std::thread destroyed on the
    // way out terminates the run.
    struct StopAsker
    {
        std::atomic<bool>& stop;
        std::thread& asker;
        ~StopAsker()
        {
            stop = true;
            if( asker.joinable() )
                asker.join();
        }
    } stopAsker{ stop, asker };

    REQUIRE( ConnectionWaits::WaitUntil(
        [&] {
            client.Pump();
            return answered.load() > 10;
        },
        kWaitBudgetMs ) );
    server->Shutdown( 0 );
    std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
    stop = true;
    asker.join();

    CHECK( longestFailure < kPromptMs );
}

#endif // RAKPEER_USER_THREADED != 1

TEST_CASE( "After Shutdown the statistics queries fail at once", "[network]" )
{
    Peer server( kAfterShutdownServerPort, 1 );
    Peer client( kAfterShutdownClientPort, 1 );
    REQUIRE( Connect( client, server ) );
    const int index = server->GetIndexFromSystemAddress( client.Address() );
    REQUIRE( index >= 0 );

    server->Shutdown( 0 );

    const TimeMS start = GetTimeMS();
    RakNetStatistics statistics;
    CHECK( server->GetStatistics( client.Address(), &statistics ) == nullptr );
    CHECK( server->GetStatistics( client.Address() ) == nullptr );
    CHECK( server->GetStatistics( UNASSIGNED_SYSTEM_ADDRESS, &statistics ) == nullptr );
    CHECK( server->GetStatistics( (unsigned int)index, &statistics ) == false );

    std::vector<SystemAddress> addresses{ client.Address() };
    std::vector<RakNetGUID> guids{ client->GetMyGUID() };
    std::vector<RakNetStatistics> list( 1 );
    server->GetStatisticsList( addresses, guids, list );
    CHECK( addresses.empty() );
    CHECK( guids.empty() );
    CHECK( list.empty() );

    CHECK( GetTimeMS() - start < kPromptMs );
}
