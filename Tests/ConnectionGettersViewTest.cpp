#include "ConnectionWaits.h"
#include "GetTime.h"
#include "RakNetStringMakers.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/*
Pins the identity and state getters to the published view that RakPeer's network thread
copies out of its connection records (ADR-0007).

The shapes pinned here:

- While a connection is open, every getter describes it the same way.
- Once it closes, none of them answers for it: GetConnectionState says IS_NOT_CONNECTED,
  never IS_DISCONNECTED, and the index, address, RakNetGUID and per-connection values are
  the "not found" ones. Only open connection records answer (ADR-0007, point 3).
- Under churn, with clients connecting and disconnecting while other threads call every
  getter, each answer is coherent: an address and a RakNetGUID returned together belong to
  one client, and within one connection the state never moves backwards.

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
*/

using namespace RakNet;

namespace {

// Ports no other test uses.
constexpr unsigned short kClosedServerPort = 62000;
constexpr unsigned short kClosedClientPort = 62001;
constexpr unsigned short kChurnServerPort = 62010;
constexpr unsigned short kChurnClientBasePort = 62011;

constexpr int kChurnClients = 4;
constexpr int kReaderThreads = 2;
constexpr int kConnectionsPerClient = 12;
// Hang guard for the whole churn, not a settle time: it normally ends in a few seconds.
constexpr TimeMS kChurnBudgetMs = 60000;
// Hang guard for each wait below.
constexpr TimeMS kWaitBudgetMs = 10000;

/// A Peer bound to 127.0.0.1 on a fixed port, that counts its update cycles and is shut
/// down before it is destroyed.
class CountedPeer
{
public:
    CountedPeer( unsigned short port, unsigned int maxConnections )
    : peer( RakPeerInterface::GetInstance() )
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
        RakPeerInterface::DestroyInstance( peer );
    }

    CountedPeer( const CountedPeer& ) = delete;
    CountedPeer& operator=( const CountedPeer& ) = delete;

    RakPeerInterface* operator->() const { return peer; }
    RakPeerInterface* Get() const { return peer; }
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

private:
    static void CountCycle( RakPeerInterface*, void* data )
    {
        ++static_cast<CountedPeer*>( data )->cycles;
    }

    RakPeerInterface* peer;
    SystemAddress address;
    std::atomic<unsigned long long> cycles{ 0 };
};

/// Polls \a condition, draining both Peers, until it holds or the budget is spent.
template<class Condition>
bool WaitFor( CountedPeer& a, CountedPeer& b, Condition condition )
{
    const TimeMS deadline = GetTimeMS() + kWaitBudgetMs;
    while( ConnectionWaits::Expired( deadline ) == false )
    {
        ConnectionWaits::Drain( a.Get() );
        ConnectionWaits::Drain( b.Get() );
        if( condition() )
            return true;
        std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
    }
    return condition();
}

bool Contains( const std::vector<SystemAddress>& addresses, const SystemAddress& address )
{
    return std::find( addresses.begin(), addresses.end(), address ) != addresses.end();
}

} // namespace

TEST_CASE( "The identity and state getters describe an open connection, and stop answering once it closes", "[network]" )
{
    CountedPeer server( kClosedServerPort, 1 );
    CountedPeer client( kClosedClientPort, 1 );
    const SystemAddress clientAddress = client.Address();
    const RakNetGUID clientGuid = client.Guid();

    REQUIRE( client->Connect( "127.0.0.1", kClosedServerPort, 0, 0 ) == CONNECTION_ATTEMPT_STARTED );
    REQUIRE( WaitFor( client, server, [&] {
        return client->GetConnectionState( server.Address() ) == IS_CONNECTED &&
               server->GetConnectionState( clientAddress ) == IS_CONNECTED;
    } ) );

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
    REQUIRE( WaitFor( client, server, [&] {
        const ConnectionState state = server->GetConnectionState( clientAddress );
        return state != IS_CONNECTED && state != IS_DISCONNECTING;
    } ) );
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

/// Records a failure if a connection's state moved backwards from \a previous to \a state,
/// or fell out of a connection attempt, which on loopback always succeeds. Returns the new
/// progress.
int CheckProgress( int previous, ConnectionState state, const std::string& who, Failures& failures )
{
    const int now = Progress( state );
    if( now != -1 && now < previous )
        failures.Add( who + "state moved backwards from " + std::to_string( previous ) + " to " + std::to_string( now ) );
    if( now == -1 && ( previous == Progress( IS_PENDING ) || previous == Progress( IS_CONNECTING ) ) )
        failures.Add( who + "state fell from " + std::to_string( previous ) + " to IS_NOT_CONNECTED during the connection attempt" );
    return now;
}

struct Client
{
    SystemAddress address;
    RakNetGUID guid;
    RakPeerInterface* peer;
};

/// Calls every getter against every client, over and over, and records anything
/// incoherent. Also follows each client's own state toward the server. \a passes counts
/// completed sweeps.
void ReadGettersUntilStopped( RakPeerInterface* server, const SystemAddress& serverAddress, const std::vector<Client>& clients, std::atomic<bool>& stop, std::atomic<unsigned long long>& passes, Failures& failures )
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
            if( server->GetTimeoutTime( client.address ) == 0 )
                failures.Add( who + "GetTimeoutTime returned 0" );
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

    std::atomic<bool> stopReading{ false };
    std::atomic<unsigned long long> passes[kReaderThreads];
    Failures failures;
    std::vector<std::thread> readers;
    for( int i = 0; i < kReaderThreads; i++ )
    {
        passes[i] = 0;
        readers.emplace_back( ReadGettersUntilStopped, server.Get(), std::cref( server.Address() ), std::cref( clients ), std::ref( stopReading ), std::ref( passes[i] ), std::ref( failures ) );
    }

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
                    // The attempt failed. Wait for the server to forget it, then try again.
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

    stopReading = true;
    for( std::thread& reader : readers )
        reader.join();

    INFO( "first failure: " << failures.First() );
    CHECK( failures.Count() == 0 );
    CHECK_FALSE( timedOut );
    for( int r = 0; r < kReaderThreads; r++ )
        CHECK( passes[r].load() > 0 );
}
