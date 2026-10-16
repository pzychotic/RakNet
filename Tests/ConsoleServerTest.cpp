#include "PeerScope.h"

#include "ConsoleServer.h"
#include "RakNetCommandParser.h"
#include "RakPeerInterface.h"
#include "TransportInterface.h"

#include "RakNetTypes.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

/*
ConsoleServer directs a command to a parser by its number when the line starts with a
digit. The number is read whole, so anything after the digits or a number past the
parsers listed is an invalid index. Driven through a transport that hands ConsoleServer
one line, with a RakNetCommandParser as parser 1 over a peer that was never started.
*/

using namespace RakNet;

namespace {

class OneLineTransport : public TransportInterface
{
public:
    std::string sent;

    explicit OneLineTransport( const char* line )
    {
        snprintf( text, sizeof( text ), "%s", line );
        packet.data = reinterpret_cast<unsigned char*>( text );
        packet.length = static_cast<unsigned int>( strlen( text ) );
        packet.systemAddress = UNASSIGNED_SYSTEM_ADDRESS;
    }

    bool Start( unsigned short, bool ) override { return true; }
    void Stop( void ) override {}
    void Send( SystemAddress, const char* data, ... ) override
    {
        char buffer[512];
        va_list args;
        va_start( args, data );
        vsnprintf( buffer, sizeof( buffer ), data, args );
        va_end( args );
        sent += buffer;
    }
    void CloseConnection( SystemAddress ) override {}
    Packet* Receive( void ) override
    {
        if( delivered )
            return nullptr;
        delivered = true;
        return &packet;
    }
    void DeallocatePacket( Packet* ) override {}
    SystemAddress HasNewIncomingConnection( void ) override { return UNASSIGNED_SYSTEM_ADDRESS; }
    SystemAddress HasLostConnection( void ) override { return UNASSIGNED_SYSTEM_ADDRESS; }
    CommandParserInterface* GetCommandParser( void ) override { return nullptr; }

private:
    char text[128];
    Packet packet{};
    bool delivered = false;
};

struct ConsoleResult
{
    std::string sent;
    unsigned int maximumIncomingConnections;
};

ConsoleResult RunLine( const char* line )
{
    PeerScope peers;
    RakPeerInterface* peer = peers.Create();
    peer->SetMaximumIncomingConnections( 3 );

    RakNetCommandParser parser;
    parser.SetRakPeerInterface( peer );
    OneLineTransport transport( line );

    ConsoleServer console;
    console.SetPrompt( "> " );
    console.SetTransportProvider( &transport, 0 );
    console.AddCommandParser( &parser );
    console.Update();

    return { transport.sent, peer->GetMaximumIncomingConnections() };
}

} // namespace

TEST_CASE( "ConsoleServer directs a command to the parser numbered first on the line", "[consoleserver]" )
{
    const ConsoleResult result = RunLine( "1 SetMaximumIncomingConnections 7" );
    CHECK( result.sent.find( "Invalid index." ) == std::string::npos );
    CHECK( result.maximumIncomingConnections == 7 );
}

TEST_CASE( "ConsoleServer refuses a parser number that is not a whole listed number", "[consoleserver]" )
{
    for( const char* line : { "2 SetMaximumIncomingConnections 7", "1x SetMaximumIncomingConnections 7", "4294967297 SetMaximumIncomingConnections 7" } )
    {
        CAPTURE( line );
        const ConsoleResult result = RunLine( line );
        CHECK( result.sent.find( "Invalid index." ) != std::string::npos );
        CHECK( result.maximumIncomingConnections == 3 );
    }
}
