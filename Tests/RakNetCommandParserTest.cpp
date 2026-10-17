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

/*
RakNetCommandParser reads a numeric argument whole, as a number in the range of the
parameter it is passed to. Anything else reads as 0, as atoi read text with no leading
digits. Driven through SetMaximumIncomingConnections, whose unsigned short argument
reads straight back, on a peer that was never started. Startup's unsigned int argument
is read by a peer that records it instead of starting.
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
