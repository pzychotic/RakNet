#include "PeerScope.h"

#include "CommonFunctions.h"
#include "GetTime.h"
#include "MessageIdentifiers.h"
#include "RakNetDefines.h"
#include "RakNetStringMakers.h"
#include "RakNetTypes.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <thread>

/*
Pins a Peer's list of internal addresses: what SetInternalID, GetInternalID and GetLocalIP do
with an index, and that the list can change while the network thread uses it.

RakPeerInterface functions explicitly tested:

    SetInternalID
    GetInternalID
    GetLocalIP
    IsLocalIP
*/

using namespace RakNet;

namespace {

constexpr unsigned short kServerPort = 30000;

// Hang guard for every wait below, not a settle time.
constexpr int kWaitBudgetMs = 5000;

const SystemAddress kFirstAddress( "10.0.0.1", 1001 );
const SystemAddress kSecondAddress( "10.0.0.2", 1002 );

} // namespace

TEST_CASE( "GetLocalIP called on two threads at once gives each thread its own string", "[network]" )
{
    PeerScope peers;
    RakPeerInterface* peer = peers.Create();
    peer->SetInternalID( kFirstAddress, 0 );
    peer->SetInternalID( kSecondAddress, 1 );

    constexpr int kCalls = 2000;
    std::atomic<bool> go{ false };
    const char* firstString = nullptr;
    const char* secondString = nullptr;
    int firstMismatches = 0;
    int secondMismatches = 0;

    auto caller = [&]( unsigned int index, const char* expected, const char*& string, int& mismatches ) {
        while( go == false )
            std::this_thread::yield();
        for( int i = 0; i < kCalls; ++i )
        {
            string = peer->GetLocalIP( index );
            if( strcmp( string, expected ) != 0 )
                ++mismatches;
        }
    };
    std::thread first( caller, 0u, "10.0.0.1", std::ref( firstString ), std::ref( firstMismatches ) );
    std::thread second( caller, 1u, "10.0.0.2", std::ref( secondString ), std::ref( secondMismatches ) );
    go = true;
    first.join();
    second.join();

    // Each thread has its own buffer, which holds even in a run where no write happened to
    // land between another thread's write and its read.
    CHECK( (const void*)firstString != (const void*)secondString );
    CHECK( firstMismatches == 0 );
    CHECK( secondMismatches == 0 );
}

TEST_CASE( "A second GetLocalIP call on the same thread reuses the first call's string", "[network]" )
{
    PeerScope peers;
    RakPeerInterface* peer = peers.Create();
    peer->SetInternalID( kFirstAddress, 0 );
    peer->SetInternalID( kSecondAddress, 1 );

    const char* first = peer->GetLocalIP( 0 );
    const char* second = peer->GetLocalIP( 1 );
    CHECK( first == second );
    CHECK( std::string( second ) == "10.0.0.2" );
}

TEST_CASE( "An internal ID index outside the list reads and writes nothing", "[network]" )
{
    PeerScope peers;
    RakPeerInterface* server = peers.Server( kServerPort );
    RakPeerInterface* client = peers.Client();
    const SystemAddress serverAddress( "127.0.0.1", kServerPort );

    server->SetInternalID( kFirstAddress, 0 );
    server->SetInternalID( kSecondAddress, 1 );
    server->SetInternalID( UNASSIGNED_SYSTEM_ADDRESS, 2 );

    CHECK( std::string( server->GetLocalIP( MAXIMUM_NUMBER_OF_INTERNAL_IDS ) ).empty() );
    CHECK( std::string( server->GetLocalIP( (unsigned int)-1 ) ).empty() );
    CHECK( server->GetInternalID( UNASSIGNED_SYSTEM_ADDRESS, MAXIMUM_NUMBER_OF_INTERNAL_IDS ) == UNASSIGNED_SYSTEM_ADDRESS );
    CHECK( server->GetInternalID( UNASSIGNED_SYSTEM_ADDRESS, -1 ) == UNASSIGNED_SYSTEM_ADDRESS );

#if !defined( _DEBUG )
    // RakAssert stops a debug build here.
    server->SetInternalID( kSecondAddress, MAXIMUM_NUMBER_OF_INTERNAL_IDS );
    server->SetInternalID( kSecondAddress, -1 );
#endif
    CHECK( server->GetInternalID( UNASSIGNED_SYSTEM_ADDRESS, 0 ) == kFirstAddress );
    CHECK( server->GetInternalID( UNASSIGNED_SYSTEM_ADDRESS, 1 ) == kSecondAddress );
    CHECK( server->GetNumberOfAddresses() == 2 );

    // A connected System's list is checked too.
    REQUIRE( client->Connect( "127.0.0.1", kServerPort, nullptr, 0 ) == CONNECTION_ATTEMPT_STARTED );
    Packet* accepted = CommonFunctions::WaitAndReturnMessageWithID( client, ID_CONNECTION_REQUEST_ACCEPTED, kWaitBudgetMs );
    REQUIRE( accepted != nullptr );
    client->DeallocatePacket( accepted );

    const TimeMS deadline = GetTimeMS() + kWaitBudgetMs;
    while( client->GetInternalID( serverAddress, 1 ) != kSecondAddress && GetTimeMS() < deadline )
        std::this_thread::yield();
    REQUIRE( client->GetInternalID( serverAddress, 1 ) == kSecondAddress );
    CHECK( client->GetInternalID( serverAddress, MAXIMUM_NUMBER_OF_INTERNAL_IDS ) == UNASSIGNED_SYSTEM_ADDRESS );
    CHECK( client->GetInternalID( serverAddress, -1 ) == UNASSIGNED_SYSTEM_ADDRESS );
}

TEST_CASE( "Connections open while the internal IDs change", "[network]" )
{
    // Exists for ThreadSanitizer: a user thread rewrites both Peers' lists while their
    // network threads write them into connection messages and match addresses against them.
    constexpr int kConnections = 8;

    PeerScope peers;
    RakPeerInterface* server = peers.Server( kServerPort );
    RakPeerInterface* client = peers.Client();
    const SystemAddress serverAddress( "127.0.0.1", kServerPort );

    // Past the addresses the machine reported, so the loopback entries stay in place.
    const unsigned int serverIndex = (std::min)( server->GetNumberOfAddresses(), (unsigned int)MAXIMUM_NUMBER_OF_INTERNAL_IDS - 1 );
    const unsigned int clientIndex = (std::min)( client->GetNumberOfAddresses(), (unsigned int)MAXIMUM_NUMBER_OF_INTERNAL_IDS - 1 );

    std::atomic<bool> churning{ true };
    int readBacksDiffering = 0;
    std::thread churner( [&] {
        bool first = true;
        while( churning )
        {
            const SystemAddress& address = first ? kFirstAddress : kSecondAddress;
            server->SetInternalID( address, (int)serverIndex );
            client->SetInternalID( address, (int)clientIndex );
            if( server->GetInternalID( UNASSIGNED_SYSTEM_ADDRESS, (int)serverIndex ) != address )
                ++readBacksDiffering;
            (void)server->IsLocalIP( "10.0.0.1" );
            (void)client->IsLocalIP( "10.0.0.2" );
            first = !first;
        }
    } );

    // A failed REQUIRE unwinds through here, and the churner uses both Peers, so it stops
    // before PeerScope destroys them.
    struct StopAndJoin
    {
        std::atomic<bool>& running;
        std::thread& thread;
        ~StopAndJoin()
        {
            running = false;
            if( thread.joinable() )
                thread.join();
        }
    } stopChurner{ churning, churner };

    for( int i = 0; i < kConnections; ++i )
    {
        INFO( "connection " << i );
        REQUIRE( client->Connect( "127.0.0.1", kServerPort, nullptr, 0 ) == CONNECTION_ATTEMPT_STARTED );
        Packet* accepted = CommonFunctions::WaitAndReturnMessageWithID( client, ID_CONNECTION_REQUEST_ACCEPTED, kWaitBudgetMs );
        REQUIRE( accepted != nullptr );
        client->DeallocatePacket( accepted );

        Packet* incoming = CommonFunctions::WaitAndReturnMessageWithID( server, ID_NEW_INCOMING_CONNECTION, kWaitBudgetMs );
        REQUIRE( incoming != nullptr );
        server->DeallocatePacket( incoming );

        client->CloseConnection( serverAddress, true );
        Packet* closed = CommonFunctions::WaitAndReturnMessageWithID( server, ID_DISCONNECTION_NOTIFICATION, kWaitBudgetMs );
        REQUIRE( closed != nullptr );
        server->DeallocatePacket( closed );

        const TimeMS deadline = GetTimeMS() + kWaitBudgetMs;
        while( client->GetConnectionState( serverAddress ) != IS_NOT_CONNECTED && GetTimeMS() < deadline )
            std::this_thread::yield();
        REQUIRE( client->GetConnectionState( serverAddress ) == IS_NOT_CONNECTED );
    }

    churning = false;
    churner.join();
    CHECK( readBacksDiffering == 0 );
}
