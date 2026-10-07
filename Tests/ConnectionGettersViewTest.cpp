#include "ConnectionWaits.h"
#include "GetTime.h"
#include "RakNetStringMakers.h"
#include "RakPeer.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/*
Pins the identity, state, ping and clock getters to the published view that RakPeer's
network thread copies out of its connection records (ADR-0007).

The shapes pinned here:

- While a connection is open, every getter describes it the same way.
- Once it closes, none of them answers for it: GetConnectionState says IS_NOT_CONNECTED,
  never IS_DISCONNECTED, and the index, address, RakNetGUID and per-connection values are
  the "not found" ones. Only open connection records answer (ADR-0007, point 3).
- Once pings have settled, the ping and clock getters answer on the user thread, by address
  and by RakNetGUID, what the network thread works out from its connection records.
- Under churn, with clients connecting and disconnecting while other threads call every
  getter and the connection-record setters, each answer is coherent: an address and a
  RakNetGUID returned together belong to one client, within one connection the state never
  moves backwards, every ping is either "none" or one loopback allows, and every timeout
  is one a setter or the default set.

RakPeerInterface functions explicitly tested:

    GetConnectionState
    GetIndexFromSystemAddress
    GetSystemAddressFromIndex
    GetGUIDFromIndex
    GetSystemList
    NumberOfConnections
    GetConnectionList
    GetGuidFromSystemAddress
    GetSystemAddressFromGuid
    GetInternalID
    GetExternalID
    GetMTUSize
    GetTimeoutTime
    GetAveragePing
    GetLastPing
    GetLowestPing
    GetClockDifferential
    SetTimeoutTime
    SetSplitMessageProgressInterval
    GetSplitMessageProgressInterval
    SetUnreliableTimeout
    ApplyNetworkSimulator
    IsNetworkSimulatorActive
*/

using namespace RakNet;

namespace {

// Ports no other test uses.
constexpr unsigned short kClosedServerPort = 32000;
constexpr unsigned short kClosedClientPort = 32001;
constexpr unsigned short kChurnServerPort = 32010;
constexpr unsigned short kChurnClientBasePort = 32011;
constexpr unsigned short kPingServerPort = 32020;
constexpr unsigned short kPingClientPort = 32021;

// What GetLastPing and GetLowestPing answer for a connection no pong has reached yet.
constexpr int kNoPingYet = 65535;
// The churn applies no network simulator, so a loopback ping is a few milliseconds. This
// is a stuck-somewhere ceiling, loose enough for a loaded machine.
constexpr int kMaxLoopbackPingMs = 1000;

constexpr int kChurnClients = 4;
constexpr int kReaderThreads = 2;
constexpr int kConnectionsPerClient = 12;
// Hang guard for the whole churn, not a settle time: it normally ends in a few seconds.
constexpr TimeMS kChurnBudgetMs = 60000;
// Hang guard for each wait below.
constexpr TimeMS kWaitBudgetMs = 10000;
// How long the ping test receives after each of its pings, and then after the last one so
// every pong is back. Settle times, not hang guards: the test receives for all of each.
constexpr TimeMS kPongWindowMs = 40;
constexpr TimeMS kPingSettleMs = 500;
// The timeouts the churn's setter thread alternates between. Both far above a loopback
// round trip, so neither drops a connection.
constexpr TimeMS kChurnTimeouts[] = { 20000, 25000 };

/// Everything the ping and clock getters answer for one remote system.
struct PingSummary
{
    int average;
    int last;
    int lowest;
    Time clockDifferential;
};

bool operator==( const PingSummary& a, const PingSummary& b )
{
    return a.average == b.average && a.last == b.last && a.lowest == b.lowest && a.clockDifferential == b.clockDifferential;
}

/// A RakPeer that can work out the ping summary straight from its connection records, which
/// are protected in RakPeer. Built directly rather than through GetInstance, so it can be
/// this subclass.
class RecordReadingPeer : public RakPeer
{
public:
    /// Network thread only. The formulas are the ones the stock getters used on the records:
    /// the mean of the filled ping slots, the slot before the write index, the lowest ping
    /// seen, and the clock differential of the lowest-ping slot.
    bool PingSummaryFromRecords( const SystemAddress& address, PingSummary& out ) const
    {
        for( unsigned int i = 0; i < GetMaximumNumberOfPeers(); i++ )
        {
            const RemoteSystemStruct& record = remoteSystemList[i];
            if( record.isActive == false || record.systemAddress != address )
                continue;

            int sum = 0;
            int filled = 0;
            int lowestSlot = kNoPingYet;
            out.clockDifferential = 0;
            for( ; filled < PING_TIMES_ARRAY_SIZE && record.pingAndClockDifferential[filled].pingTime != kNoPingYet; filled++ )
            {
                sum += record.pingAndClockDifferential[filled].pingTime;
                if( record.pingAndClockDifferential[filled].pingTime < lowestSlot )
                {
                    lowestSlot = record.pingAndClockDifferential[filled].pingTime;
                    out.clockDifferential = record.pingAndClockDifferential[filled].clockDifferential;
                }
            }
            out.average = filled > 0 ? sum / filled : -1;
            const unsigned int lastSlot = ( (unsigned int)record.pingAndClockDifferentialWriteIndex + PING_TIMES_ARRAY_SIZE - 1 ) % PING_TIMES_ARRAY_SIZE;
            out.last = record.pingAndClockDifferential[lastSlot].pingTime;
            out.lowest = record.lowestPing;
            return true;
        }
        return false;
    }
};

/// A Peer bound to 127.0.0.1 on a fixed port, that counts its update cycles and is shut
/// down before it is destroyed.
class CountedPeer
{
public:
    CountedPeer( unsigned short port, unsigned int maxConnections )
    : peer( new RecordReadingPeer )
    {
        SocketDescriptor socketDescriptor( port, "127.0.0.1" );
        REQUIRE( peer->Startup( maxConnections, &socketDescriptor, 1 ) == RAKNET_STARTED );
        peer->SetMaximumIncomingConnections( (unsigned short)maxConnections );
        peer->SetUserUpdateThread( &CountedPeer::CountCycle, this );
        address.FromStringExplicitPort( "127.0.0.1", port );
    }

    ~CountedPeer()
    {
        // The network thread calls CountCycle, so it has to be gone before this is.
        peer->Shutdown( 0 );
    }

    CountedPeer( const CountedPeer& ) = delete;
    CountedPeer& operator=( const CountedPeer& ) = delete;

    RakPeerInterface* operator->() const { return peer.get(); }
    RakPeerInterface* Get() const { return peer.get(); }
    const RecordReadingPeer& Records() const { return *peer; }
    const SystemAddress& Address() const { return address; }
    RakNetGUID Guid() const { return peer->GetMyGUID(); }

    /// Blocks until an update cycle that started after this call has finished, so the view
    /// holds whatever the connection records held at the call. The callback runs at the
    /// start of each cycle, so the second one counted after the call ends the first cycle
    /// that started after it.
    bool WaitForAFullCycle() const
    {
        const unsigned long long target = cycles.load() + 2;
        const TimeMS deadline = GetTimeMS() + kWaitBudgetMs;
        while( cycles.load() < target )
        {
            if( ConnectionWaits::Expired( deadline ) )
                return false;
            std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
        }
        return true;
    }

    /// Runs \a work on the network thread, between two update cycles, and blocks until it
    /// has run. Nothing else touches the connection records while it runs.
    bool RunOnNetworkThread( std::function<void()> work )
    {
        {
            std::lock_guard<std::mutex> guard( workMutex );
            pendingWork = std::move( work );
        }
        const TimeMS deadline = GetTimeMS() + kWaitBudgetMs;
        for( ;; )
        {
            {
                std::lock_guard<std::mutex> guard( workMutex );
                if( !pendingWork )
                    return true;
                if( ConnectionWaits::Expired( deadline ) )
                {
                    pendingWork = nullptr;
                    return false;
                }
            }
            std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
        }
    }

private:
    static void CountCycle( RakPeerInterface*, void* data )
    {
        CountedPeer* self = static_cast<CountedPeer*>( data );
        {
            std::lock_guard<std::mutex> guard( self->workMutex );
            if( self->pendingWork )
            {
                self->pendingWork();
                self->pendingWork = nullptr;
            }
        }
        ++self->cycles;
    }

    std::unique_ptr<RecordReadingPeer> peer;
    SystemAddress address;
    std::atomic<unsigned long long> cycles{ 0 };
    std::mutex workMutex;
    std::function<void()> pendingWork;
};

bool Contains( const std::vector<SystemAddress>& addresses, const SystemAddress& address )
{
    return std::find( addresses.begin(), addresses.end(), address ) != addresses.end();
}

} // namespace

TEST_CASE( "The identity and state getters describe an open connection, and stop answering once it closes", "[network]" )
{
    CountedPeer server( kClosedServerPort, 1 );
    CountedPeer client( kClosedClientPort, 1 );
    RakPeerInterface* const both[] = { client.Get(), server.Get() };
    const SystemAddress clientAddress = client.Address();
    const RakNetGUID clientGuid = client.Guid();

    REQUIRE( client->Connect( "127.0.0.1", kClosedServerPort, 0, 0 ) == CONNECTION_ATTEMPT_STARTED );
    REQUIRE( ConnectionWaits::DrainUntil(
        both, 2,
        [&] {
            return client->GetConnectionState( server.Address() ) == IS_CONNECTED &&
                   server->GetConnectionState( clientAddress ) == IS_CONNECTED;
        },
        kWaitBudgetMs ) );

    const int index = server->GetIndexFromSystemAddress( clientAddress );
    REQUIRE( index >= 0 );

    {
        INFO( "while the connection is open" );
        CHECK( server->GetConnectionState( clientGuid ) == IS_CONNECTED );
        CHECK( server->GetSystemAddressFromIndex( (unsigned int)index ) == clientAddress );
        CHECK( server->GetGUIDFromIndex( (unsigned int)index ) == clientGuid );
        CHECK( server->GetSystemAddressFromGuid( clientGuid ) == clientAddress );

        const RakNetGUID guid = server->GetGuidFromSystemAddress( clientAddress );
        CHECK( guid == clientGuid );
        CHECK( guid.systemIndex == (SystemIndex)index );

        std::vector<SystemAddress> addresses;
        std::vector<RakNetGUID> guids;
        server->GetSystemList( addresses, guids );
        REQUIRE( addresses.size() == 1 );
        REQUIRE( guids.size() == 1 );
        CHECK( addresses[0] == clientAddress );
        CHECK( guids[0] == clientGuid );
        CHECK( server->NumberOfConnections() == 1 );

        SystemAddress list[2];
        unsigned short listSize = 2;
        CHECK( server->GetConnectionList( list, &listSize ) );
        CHECK( listSize == 1 );
        CHECK( list[0] == clientAddress );

        CHECK( server->GetExternalID( clientAddress ).GetPort() == kClosedServerPort );
        CHECK( server->GetMTUSize( clientAddress ) > 0 );
        CHECK( server->GetTimeoutTime( clientAddress ) == server->GetTimeoutTime( UNASSIGNED_SYSTEM_ADDRESS ) );
    }

    client->CloseConnection( server.Address(), true, 0, LOW_PRIORITY );
    REQUIRE( ConnectionWaits::DrainUntil(
        both, 2,
        [&] {
            const ConnectionState state = server->GetConnectionState( clientAddress );
            return state != IS_CONNECTED && state != IS_DISCONNECTING;
        },
        kWaitBudgetMs ) );
    REQUIRE( server.WaitForAFullCycle() );

    {
        INFO( "once the connection has closed" );
        CHECK( server->GetConnectionState( clientAddress ) == IS_NOT_CONNECTED );
        CHECK( server->GetConnectionState( clientGuid ) == IS_NOT_CONNECTED );
        CHECK( server->GetIndexFromSystemAddress( clientAddress ) == -1 );
        CHECK( server->GetSystemAddressFromIndex( (unsigned int)index ) == UNASSIGNED_SYSTEM_ADDRESS );
        CHECK( server->GetGUIDFromIndex( (unsigned int)index ) == UNASSIGNED_RAKNET_GUID );
        CHECK( server->GetSystemAddressFromGuid( clientGuid ) == UNASSIGNED_SYSTEM_ADDRESS );
        CHECK( server->GetGuidFromSystemAddress( clientAddress ) == UNASSIGNED_RAKNET_GUID );
        CHECK( server->NumberOfConnections() == 0 );
        CHECK( server->GetInternalID( clientAddress ) == UNASSIGNED_SYSTEM_ADDRESS );
        CHECK( server->GetExternalID( clientAddress ) == UNASSIGNED_SYSTEM_ADDRESS );
        CHECK( server->GetMTUSize( clientAddress ) == server->GetMTUSize( UNASSIGNED_SYSTEM_ADDRESS ) );
        CHECK( server->GetTimeoutTime( clientAddress ) == server->GetTimeoutTime( UNASSIGNED_SYSTEM_ADDRESS ) );
    }
}

namespace {

PingSummary ReadPingSummary( RakPeerInterface* peer, const AddressOrGUID& systemIdentifier )
{
    return PingSummary{ peer->GetAveragePing( systemIdentifier ), peer->GetLastPing( systemIdentifier ), peer->GetLowestPing( systemIdentifier ),
                        peer->GetClockDifferential( systemIdentifier ) };
}

} // namespace

TEST_CASE( "The ping and clock getters answer on the user thread what the network thread computes", "[network]" )
{
    CountedPeer server( kPingServerPort, 1 );
    CountedPeer client( kPingClientPort, 1 );
    RakPeerInterface* const both[] = { client.Get(), server.Get() };
    const SystemAddress clientAddress = client.Address();
    const RakNetGUID clientGuid = client.Guid();

    // Delays the client's pongs, so the server's pings differ from each other and from 0.
    // The simulator only runs in Debug builds.
    client->ApplyNetworkSimulator( 0.0f, 10, 20 );

    REQUIRE( client->Connect( "127.0.0.1", kPingServerPort, 0, 0 ) == CONNECTION_ATTEMPT_STARTED );
    REQUIRE( ConnectionWaits::DrainUntil(
        both, 2,
        [&] {
            return client->GetConnectionState( server.Address() ) == IS_CONNECTED &&
                   server->GetConnectionState( clientAddress ) == IS_CONNECTED;
        },
        kWaitBudgetMs ) );

    // Occasional pings are off, so once these pongs are back the server's ping records hold
    // still.
    for( int i = 0; i < 6; i++ )
    {
        server->Ping( clientAddress );
        ConnectionWaits::DrainUntil( both, 2, [] { return false; }, kPongWindowMs );
    }
    ConnectionWaits::DrainUntil( both, 2, [] { return false; }, kPingSettleMs );
    REQUIRE( server.WaitForAFullCycle() );

    PingSummary fromRecords{};
    bool foundInRecords = false;
    REQUIRE( server.RunOnNetworkThread( [&] { foundInRecords = server.Records().PingSummaryFromRecords( clientAddress, fromRecords ); } ) );
    REQUIRE( foundInRecords );
    const PingSummary byAddress = ReadPingSummary( server.Get(), clientAddress );
    const PingSummary byGuid = ReadPingSummary( server.Get(), clientGuid );

    {
        INFO( "while the connection is open" );
        CHECK( fromRecords.last != kNoPingYet );
        CHECK( fromRecords.average >= 0 );
        CHECK( fromRecords.lowest <= fromRecords.last );
        CHECK( fromRecords.lowest <= fromRecords.average );
#ifndef FLIP_SEND_ORDER_TEST
        // FLIP_SEND_ORDER_TEST turns the simulator's latency into a reordering and adds no
        // delay, so there is no floor in that mode.
        if( client->IsNetworkSimulatorActive() )
            CHECK( fromRecords.lowest >= 10 );
#endif

        CHECK( byAddress.average == fromRecords.average );
        CHECK( byAddress.last == fromRecords.last );
        CHECK( byAddress.lowest == fromRecords.lowest );
        CHECK( byAddress.clockDifferential == fromRecords.clockDifferential );
        CHECK( byGuid == byAddress );
    }

    client->CloseConnection( server.Address(), true, 0, LOW_PRIORITY );
    REQUIRE( ConnectionWaits::DrainUntil( both, 2, [&] { return server->GetConnectionState( clientAddress ) == IS_NOT_CONNECTED; }, kWaitBudgetMs ) );
    REQUIRE( server.WaitForAFullCycle() );

    {
        INFO( "once the connection has closed" );
        const PingSummary none{ -1, -1, -1, 0 };
        CHECK( ReadPingSummary( server.Get(), clientAddress ) == none );
        CHECK( ReadPingSummary( server.Get(), clientGuid ) == none );
    }
}

namespace {

/// What the churn readers found wrong. Catch2's assertions are not thread safe, so readers
/// record here and the test thread checks afterwards.
class Failures
{
public:
    void Add( const std::string& failure )
    {
        std::lock_guard<std::mutex> guard( mutex );
        ++count;
        if( first.empty() )
            first = failure;
    }

    int Count()
    {
        std::lock_guard<std::mutex> guard( mutex );
        return count;
    }

    std::string First()
    {
        std::lock_guard<std::mutex> guard( mutex );
        return first;
    }

private:
    std::mutex mutex;
    int count = 0;
    std::string first;
};

/// How far a connection has got. -1 means gone, and a new connection may start from there.
/// Anything else may only stay or rise.
int Progress( ConnectionState state )
{
    switch( state )
    {
    case IS_PENDING:
        return 0;
    case IS_CONNECTING:
        return 1;
    case IS_CONNECTED:
        return 2;
    case IS_DISCONNECTING:
    case IS_SILENTLY_DISCONNECTING:
        return 3;
    default:
        return -1;
    }
}

/// Records a failure if a connection's state moved backwards from \a previous to \a state.
/// Returns the new progress.
///
/// A reader samples, so it can miss every state between two reads. IS_CONNECTING followed
/// by IS_NOT_CONNECTED may be a whole connection it never saw, so a failed attempt is
/// caught by the churn loop instead, which watches the client's own state.
int CheckProgress( int previous, ConnectionState state, const std::string& who, Failures& failures )
{
    const int now = Progress( state );
    if( now != -1 && now < previous )
        failures.Add( who + "state moved backwards from " + std::to_string( previous ) + " to " + std::to_string( now ) );
    return now;
}

bool IsLoopbackPing( int ping )
{
    return ping >= 0 && ping <= kMaxLoopbackPingMs;
}

/// Records a failure for any ping or clock getter answering outside what loopback allows.
/// -1 means not connected, and kNoPingYet means connected with no pong back yet.
void CheckPings( RakPeerInterface* server, const AddressOrGUID& systemIdentifier, const std::string& who, Failures& failures )
{
    const PingSummary pings = ReadPingSummary( server, systemIdentifier );
    if( pings.average != -1 && IsLoopbackPing( pings.average ) == false )
        failures.Add( who + "GetAveragePing returned " + std::to_string( pings.average ) );
    if( pings.last != -1 && pings.last != kNoPingYet && IsLoopbackPing( pings.last ) == false )
        failures.Add( who + "GetLastPing returned " + std::to_string( pings.last ) );
    if( pings.lowest != -1 && pings.lowest != kNoPingYet && IsLoopbackPing( pings.lowest ) == false )
        failures.Add( who + "GetLowestPing returned " + std::to_string( pings.lowest ) );
    // Both Peers share one clock, so the differential is within a ping of 0.
    const int64_t clockDifferential = static_cast<int64_t>( pings.clockDifferential );
    if( clockDifferential < -kMaxLoopbackPingMs || clockDifferential > kMaxLoopbackPingMs )
        failures.Add( who + "GetClockDifferential returned " + std::to_string( clockDifferential ) );
}

struct Client
{
    SystemAddress address;
    RakNetGUID guid;
    RakPeerInterface* peer;
};

/// Whether \a timeout is the Peer's initial default, \a initialTimeout, or one the setter
/// thread sets.
bool IsExpectedTimeout( TimeMS timeout, TimeMS initialTimeout )
{
    return timeout == initialTimeout || std::find( std::begin( kChurnTimeouts ), std::end( kChurnTimeouts ), timeout ) != std::end( kChurnTimeouts );
}

/// Calls every getter against every client, over and over, and records anything
/// incoherent. Also follows each client's own state toward the server. \a passes counts
/// completed sweeps.
void ReadGettersUntilStopped( RakPeerInterface* server, const SystemAddress& serverAddress, const std::vector<Client>& clients, TimeMS initialTimeout, std::atomic<bool>& stop, std::atomic<unsigned long long>& passes, Failures& failures )
{
    const unsigned short serverPort = serverAddress.GetPort();
    std::vector<int> progress( clients.size(), -1 );
    std::vector<int> clientProgress( clients.size(), -1 );
    std::vector<SystemAddress> addresses;
    std::vector<RakNetGUID> guids;

    auto anyClient = [&]( auto matches ) { return std::any_of( clients.begin(), clients.end(), matches ); };
    auto isClientPair = [&]( const SystemAddress& address, const RakNetGUID& guid ) {
        return anyClient( [&]( const Client& client ) { return client.address == address && client.guid == guid; } );
    };
    auto isClientAddress = [&]( const SystemAddress& address ) {
        return anyClient( [&]( const Client& client ) { return client.address == address; } );
    };
    auto isClientGuid = [&]( const RakNetGUID& guid ) {
        return anyClient( [&]( const Client& client ) { return client.guid == guid; } );
    };

    while( stop.load() == false )
    {
        for( size_t i = 0; i < clients.size(); i++ )
        {
            const Client& client = clients[i];
            const std::string who = "client " + std::to_string( i ) + ": ";

            const ConnectionState state = server->GetConnectionState( client.address );
            if( state == IS_DISCONNECTED || state == IS_PENDING )
                failures.Add( who + "GetConnectionState by address returned " + std::to_string( (int)state ) );
            progress[i] = CheckProgress( progress[i], state, who + "server's ", failures );

            const ConnectionState toServer = client.peer->GetConnectionState( serverAddress );
            if( toServer == IS_DISCONNECTED )
                failures.Add( who + "GetConnectionState toward the server returned IS_DISCONNECTED" );
            clientProgress[i] = CheckProgress( clientProgress[i], toServer, who + "client's ", failures );

            const ConnectionState stateByGuid = server->GetConnectionState( client.guid );
            if( stateByGuid == IS_DISCONNECTED || stateByGuid == IS_PENDING )
                failures.Add( who + "GetConnectionState by RakNetGUID returned " + std::to_string( (int)stateByGuid ) );

            const RakNetGUID guid = server->GetGuidFromSystemAddress( client.address );
            if( guid != UNASSIGNED_RAKNET_GUID && guid != client.guid )
                failures.Add( who + "GetGuidFromSystemAddress returned another client's RakNetGUID" );

            const SystemAddress address = server->GetSystemAddressFromGuid( client.guid );
            if( address != UNASSIGNED_SYSTEM_ADDRESS && address != client.address )
                failures.Add( who + "GetSystemAddressFromGuid returned another client's address" );

            const int index = server->GetIndexFromSystemAddress( client.address );
            if( index < -1 || index >= kChurnClients )
                failures.Add( who + "GetIndexFromSystemAddress returned " + std::to_string( index ) );
            if( index >= 0 )
            {
                // The two calls take two snapshots, so the slot may have changed hands in
                // between. Each answer must still be some client's.
                const SystemAddress atIndex = server->GetSystemAddressFromIndex( (unsigned int)index );
                if( atIndex != UNASSIGNED_SYSTEM_ADDRESS && isClientAddress( atIndex ) == false )
                    failures.Add( who + "GetSystemAddressFromIndex returned an address no client has" );
                const RakNetGUID guidAtIndex = server->GetGUIDFromIndex( (unsigned int)index );
                if( guidAtIndex != UNASSIGNED_RAKNET_GUID && isClientGuid( guidAtIndex ) == false )
                    failures.Add( who + "GetGUIDFromIndex returned a RakNetGUID no client has" );
            }

            // GetInternalID answers with whatever the client reported, so only its presence
            // is predictable.
            (void)server->GetInternalID( client.address );

            const SystemAddress externalId = server->GetExternalID( client.address );
            if( externalId != UNASSIGNED_SYSTEM_ADDRESS && externalId.GetPort() != serverPort )
                failures.Add( who + "GetExternalID returned port " + std::to_string( externalId.GetPort() ) );

            if( server->GetMTUSize( client.address ) <= 0 )
                failures.Add( who + "GetMTUSize returned no MTU" );
            const TimeMS timeout = server->GetTimeoutTime( client.address );
            if( IsExpectedTimeout( timeout, initialTimeout ) == false )
                failures.Add( who + "GetTimeoutTime returned " + std::to_string( timeout ) );

            CheckPings( server, client.address, who + "by address: ", failures );
            CheckPings( server, client.guid, who + "by RakNetGUID: ", failures );
        }

        server->GetSystemList( addresses, guids );
        if( addresses.size() != guids.size() )
            failures.Add( "GetSystemList returned lists of different sizes" );
        else if( addresses.size() > clients.size() )
            failures.Add( "GetSystemList returned more connections than there are clients" );
        else
        {
            for( size_t j = 0; j < addresses.size(); j++ )
                if( isClientPair( addresses[j], guids[j] ) == false )
                    failures.Add( "GetSystemList paired an address with another client's RakNetGUID" );
        }

        if( server->NumberOfConnections() > clients.size() )
            failures.Add( "NumberOfConnections exceeded the number of clients" );

        SystemAddress list[kChurnClients];
        unsigned short listSize = kChurnClients;
        server->GetConnectionList( list, &listSize );
        for( unsigned short j = 0; j < listSize; j++ )
            if( isClientAddress( list[j] ) == false )
                failures.Add( "GetConnectionList returned an address no client has" );

        ++passes;
    }
}

/// Calls the connection-record setters on \a server, for every connection and for each
/// client's, over and over, and records any Peer-wide value that reads back wrong. The
/// values leave every connection working: long timeouts, and a simulator that does
/// nothing.
void CallSettersUntilStopped( RakPeerInterface* server, const std::vector<Client>& clients, TimeMS initialTimeout, std::atomic<bool>& stop, Failures& failures )
{
    for( unsigned int round = 0; stop.load() == false; round++ )
    {
        const TimeMS timeout = kChurnTimeouts[round % 2];
        server->SetTimeoutTime( timeout, UNASSIGNED_SYSTEM_ADDRESS );
        for( const Client& client : clients )
            server->SetTimeoutTime( kChurnTimeouts[( round + 1 ) % 2], client.address );
        if( server->GetTimeoutTime( UNASSIGNED_SYSTEM_ADDRESS ) != timeout )
            failures.Add( "GetTimeoutTime( UNASSIGNED_SYSTEM_ADDRESS ) returned " + std::to_string( server->GetTimeoutTime( UNASSIGNED_SYSTEM_ADDRESS ) ) );
        for( const Client& client : clients )
        {
            const TimeMS clientTimeout = server->GetTimeoutTime( client.address );
            if( IsExpectedTimeout( clientTimeout, initialTimeout ) == false )
                failures.Add( "setter thread: GetTimeoutTime returned " + std::to_string( clientTimeout ) );
        }

        const int interval = (int)( round % 2 );
        server->SetSplitMessageProgressInterval( interval );
        if( server->GetSplitMessageProgressInterval() != interval )
            failures.Add( "GetSplitMessageProgressInterval returned " + std::to_string( server->GetSplitMessageProgressInterval() ) );

        server->SetUnreliableTimeout( round % 2 == 0 ? 0 : 1000 );

        server->ApplyNetworkSimulator( 0.0f, 0, 0 );
        if( server->IsNetworkSimulatorActive() )
            failures.Add( "IsNetworkSimulatorActive returned true for a simulator that does nothing" );

        // Paced, so the commands never outrun a network thread that cycles every few
        // milliseconds.
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }
}

/// Where one client is in its connect-close loop.
enum class Phase
{
    Idle,
    Connecting,
    Closing,
    WaitingForReaders
};

struct Churn
{
    Phase phase = Phase::Idle;
    int connections = 0;
    /// Each reader's pass count that proves it has swept the client since it closed.
    unsigned long long passesToWaitFor[kReaderThreads] = {};
};

} // namespace

TEST_CASE( "Getters stay coherent while clients connect and disconnect under them", "[network]" )
{
    CountedPeer server( kChurnServerPort, kChurnClients );

    std::vector<std::unique_ptr<CountedPeer>> clientPeers;
    std::vector<Client> clients;
    for( int i = 0; i < kChurnClients; i++ )
    {
        clientPeers.emplace_back( new CountedPeer( (unsigned short)( kChurnClientBasePort + i ), 1 ) );
        clients.push_back( Client{ clientPeers.back()->Address(), clientPeers.back()->Guid(), clientPeers.back()->Get() } );
    }

    const TimeMS initialTimeout = server->GetTimeoutTime( UNASSIGNED_SYSTEM_ADDRESS );
    std::atomic<bool> stopThreads{ false };
    std::atomic<unsigned long long> passes[kReaderThreads];
    Failures failures;
    std::vector<std::thread> threads;
    for( int i = 0; i < kReaderThreads; i++ )
    {
        passes[i] = 0;
        threads.emplace_back( ReadGettersUntilStopped, server.Get(), std::cref( server.Address() ), std::cref( clients ), initialTimeout, std::ref( stopThreads ), std::ref( passes[i] ), std::ref( failures ) );
    }
    threads.emplace_back( CallSettersUntilStopped, server.Get(), std::cref( clients ), initialTimeout, std::ref( stopThreads ), std::ref( failures ) );

    // Each client connects and disconnects kConnectionsPerClient times. Between two
    // connections it waits until the server's view has lost it and every reader has
    // swept it since, so a reader sees every connection end before the next begins, and
    // can tell one connection's states from the next.
    const TimeMS deadline = GetTimeMS() + kChurnBudgetMs;
    std::vector<Churn> churn( kChurnClients );
    bool timedOut = false;

    for( ;; )
    {
        bool done = true;
        ConnectionWaits::Drain( server.Get() );
        for( int i = 0; i < kChurnClients; i++ )
        {
            CountedPeer& client = *clientPeers[i];
            Churn& state = churn[i];
            ConnectionWaits::Drain( client.Get() );
            if( state.connections >= kConnectionsPerClient )
                continue;
            done = false;

            const ConnectionState toServer = client->GetConnectionState( server.Address() );
            switch( state.phase )
            {
            case Phase::Idle:
                if( toServer == IS_NOT_CONNECTED && client->Connect( "127.0.0.1", kChurnServerPort, 0, 0 ) == CONNECTION_ATTEMPT_STARTED )
                    state.phase = Phase::Connecting;
                break;
            case Phase::Connecting:
                if( toServer == IS_CONNECTED )
                {
                    state.phase = Phase::Closing;
                    client->CloseConnection( server.Address(), true, 0, LOW_PRIORITY );
                }
                else if( toServer == IS_NOT_CONNECTED )
                {
                    // The attempt failed, which on loopback it never should. Only this loop
                    // closes the connection, and only after seeing IS_CONNECTED, so this
                    // read cannot be a connection that came and went unseen. Wait for the
                    // server to forget it, then try again.
                    failures.Add( "client " + std::to_string( i ) + ": the connection attempt failed" );
                    state.phase = Phase::Closing;
                    --state.connections;
                }
                break;
            case Phase::Closing:
                if( toServer == IS_NOT_CONNECTED && server->GetIndexFromSystemAddress( client.Address() ) == -1 &&
                    server->GetConnectionState( client.Address() ) == IS_NOT_CONNECTED )
                {
                    ++state.connections;
                    for( int r = 0; r < kReaderThreads; r++ )
                        state.passesToWaitFor[r] = passes[r].load() + 2;
                    state.phase = Phase::WaitingForReaders;
                }
                break;
            case Phase::WaitingForReaders: {
                bool swept = true;
                for( int r = 0; r < kReaderThreads; r++ )
                    swept = swept && passes[r].load() >= state.passesToWaitFor[r];
                if( swept )
                    state.phase = Phase::Idle;
                break;
            }
            }
        }

        if( done )
            break;
        if( ConnectionWaits::Expired( deadline ) )
        {
            timedOut = true;
            break;
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( 2 ) );
    }

    stopThreads = true;
    for( std::thread& thread : threads )
        thread.join();

    INFO( "first failure: " << failures.First() );
    CHECK( failures.Count() == 0 );
    CHECK_FALSE( timedOut );
    for( int r = 0; r < kReaderThreads; r++ )
        CHECK( passes[r].load() > 0 );
}
