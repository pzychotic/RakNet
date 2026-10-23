#include "PeerScope.h"

#include "RakNetCommandParser.h"
#include "RakPeer.h"
#include "RakPeerInterface.h"
#include "TransportInterface.h"

#include "RakNetTypes.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

/*
RakNetCommandParser reads a numeric argument whole, as a number in the range of the
parameter it is passed to. Anything else reads as 0, as atoi read text with no leading
digits. Driven through SetMaximumIncomingConnections, whose unsigned short argument
reads straight back, on a peer that was never started. Startup's unsigned int argument
is read by a peer that records it instead of starting.

Each command it dispatches, run on a started peer, sends a reply naming the command. A
name that differs from a command only in case is no command.
*/

using namespace RakNet;

namespace {

class NullTransport : public TransportInterface
{
public:
    bool Start( unsigned short, bool ) override { return true; }
    void Stop( void ) override {}
    void Send( SystemAddress, const char*, ... ) override {}
    void CloseConnection( SystemAddress ) override {}
    Packet* Receive( void ) override { return nullptr; }
    void DeallocatePacket( Packet* ) override {}
    SystemAddress HasNewIncomingConnection( void ) override { return UNASSIGNED_SYSTEM_ADDRESS; }
    SystemAddress HasLostConnection( void ) override { return UNASSIGNED_SYSTEM_ADDRESS; }
    CommandParserInterface* GetCommandParser( void ) override { return nullptr; }
};

// Collects everything sent, in order.
class RecordingTransport : public NullTransport
{
public:
    std::string sent;

    void Send( SystemAddress, const char* data, ... ) override
    {
        char buffer[512];
        va_list args;
        va_start( args, data );
        vsnprintf( buffer, sizeof( buffer ), data, args );
        va_end( args );
        sent += buffer;
    }
};

struct DispatchedCommand
{
    const char* name;
    std::vector<std::string> parameters;
};

// What parser sends back for command, run with its parameters.
std::string ReplyTo( RakNetCommandParser& parser, const DispatchedCommand& command )
{
    std::vector<std::string> parameters = command.parameters;
    std::vector<char*> parameterList;
    for( std::string& parameter : parameters )
        parameterList.push_back( parameter.data() );

    RecordingTransport transport;
    parser.OnCommand( command.name, static_cast<unsigned>( parameterList.size() ), parameterList.data(), &transport, UNASSIGNED_SYSTEM_ADDRESS, "" );
    return transport.sent;
}

unsigned short SetMaximumIncomingConnectionsTo( const char* argument )
{
    PeerScope peers;
    RakPeerInterface* peer = peers.Create();
    peer->SetMaximumIncomingConnections( 3 );

    RakNetCommandParser parser;
    parser.SetRakPeerInterface( peer );
    NullTransport transport;

    char parameter[32];
    snprintf( parameter, sizeof( parameter ), "%s", argument );
    char* parameterList[] = { parameter };
    parser.OnCommand( "SetMaximumIncomingConnections", 1, parameterList, &transport, UNASSIGNED_SYSTEM_ADDRESS, "" );
    return peer->GetMaximumIncomingConnections();
}

/// Records the maxConnections Startup is called with, and starts nothing.
class StartupRecordingPeer : public RakPeer
{
public:
    StartupResult Startup( unsigned int maxConnections, SocketDescriptor*, unsigned, int ) override
    {
        startedWith = maxConnections;
        return RAKNET_STARTED;
    }

    unsigned int startedWith = 0;
};

unsigned int StartupMaxConnections( const char* argument )
{
    StartupRecordingPeer peer;
    RakNetCommandParser parser;
    parser.SetRakPeerInterface( &peer );
    NullTransport transport;

    char maxConnections[32];
    snprintf( maxConnections, sizeof( maxConnections ), "%s", argument );
    char localPort[] = "0";
    char forceHostAddress[] = "127.0.0.1";
    char* parameterList[] = { maxConnections, localPort, forceHostAddress };
    parser.OnCommand( "Startup", 3, parameterList, &transport, UNASSIGNED_SYSTEM_ADDRESS, "" );
    return peer.startedWith;
}

} // namespace

TEST_CASE( "RakNetCommandParser reads a numeric argument", "[commandparser]" )
{
    CHECK( SetMaximumIncomingConnectionsTo( "7" ) == 7 );
    CHECK( SetMaximumIncomingConnectionsTo( "0" ) == 0 );
    CHECK( SetMaximumIncomingConnectionsTo( "65535" ) == 65535 );
}

TEST_CASE( "RakNetCommandParser reads an argument that is not a whole number in range as 0", "[commandparser]" )
{
    for( const char* argument : { "70000", "-1", "7x", "+7", "x" } )
    {
        CAPTURE( argument );
        CHECK( SetMaximumIncomingConnectionsTo( argument ) == 0 );
    }
}

TEST_CASE( "RakNetCommandParser passes Startup a maxConnections above 65535 whole", "[commandparser]" )
{
    CHECK( StartupMaxConnections( "8" ) == 8 );
    CHECK( StartupMaxConnections( "65537" ) == 65537 );
}

TEST_CASE( "RakNetCommandParser replies to each command it dispatches", "[commandparser]" )
{
    PeerScope peers;
    RakPeerInterface* peer = peers.Client();
    RakNetCommandParser parser;
    parser.SetRakPeerInterface( peer );

    // Disconnect shuts the peer down, so it runs last.
    const DispatchedCommand commands[] = {
        { "SetMaximumIncomingConnections", { "4" } },
        { "GetMaximumIncomingConnections", {} },
        { "Connect", { "127.0.0.1", "1", "", "0" } },
        { "IsActive", {} },
        { "GetConnectionList", {} },
        { "CloseConnection", { "127.0.0.1", "0", "0" } },
        { "GetConnectionState", { "127.0.0.1" } },
        { "GetIndexFromSystemAddress", { "127.0.0.1" } },
        { "GetSystemAddressFromIndex", { "0" } },
        { "AddToBanList", { "10.0.0.1", "0" } },
        { "IsBanned", { "10.0.0.1" } },
        { "RemoveFromBanList", { "10.0.0.1" } },
        { "ClearBanList", {} },
        { "Ping1", { "127.0.0.1" } },
        { "Ping2", { "127.0.0.1", "1", "0" } },
        { "GetAveragePing", { "127.0.0.1" } },
        { "GetLastPing", { "127.0.0.1" } },
        { "GetLowestPing", { "127.0.0.1" } },
        { "SetOccasionalPing", { "1" } },
        { "SetOfflinePingResponse", { "pong", "4" } },
        { "GetInternalID", {} },
        { "GetExternalID", { "127.0.0.1" } },
        { "SetTimeoutTime", { "1000", "127.0.0.1" } },
        { "GetMTUSize", {} },
        { "GetNumberOfAddresses", {} },
        { "GetLocalIP", { "0" } },
        { "AllowConnectionResponseIPMigration", { "0" } },
        { "AdvertiseSystem", { "127.0.0.1", "1", "", "0" } },
        { "SetIncomingPassword", { "pw", "2" } },
        { "GetIncomingPassword", {} },
        { "Disconnect", { "0", "0" } },
    };
    for( const DispatchedCommand& command : commands )
    {
        CAPTURE( command.name );
        CHECK( ReplyTo( parser, command ).find( command.name ) != std::string::npos );
    }
}

TEST_CASE( "RakNetCommandParser dispatches a command only by its exact name", "[commandparser]" )
{
    PeerScope peers;
    RakPeerInterface* peer = peers.Create();
    peer->SetMaximumIncomingConnections( 3 );
    RakNetCommandParser parser;
    parser.SetRakPeerInterface( peer );

    CHECK( ReplyTo( parser, { "setmaximumincomingconnections", { "7" } } ).empty() );
    CHECK( ReplyTo( parser, { "SetMaximumIncomingConnection", { "7" } } ).empty() );
    CHECK( peer->GetMaximumIncomingConnections() == 3 );
}
