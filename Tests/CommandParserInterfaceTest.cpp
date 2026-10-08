#include "CommandParserInterface.h"
#include "TransportInterface.h"

#include "RakNetTypes.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdarg>
#include <cstdio>
#include <string>

/*
CommandParserInterface finds a registered command by name without regard to case, and lists
its commands in case-insensitive name order.
*/

using namespace RakNet;

namespace {

class TestParser : public CommandParserInterface
{
public:
    const char* GetName( void ) const override { return "Test"; }
    void SendHelp( TransportInterface*, const SystemAddress& ) override {}
    bool OnCommand( const char*, unsigned, char**, TransportInterface*, const SystemAddress&, const char* ) override { return true; }
};

// Collects everything sent, in order.
class RecordingTransport : public TransportInterface
{
public:
    std::string sent;

    bool Start( unsigned short, bool ) override { return true; }
    void Stop( void ) override {}
    void Send( SystemAddress, const char* data, ... ) override
    {
        char buffer[256];
        va_list args;
        va_start( args, data );
        vsnprintf( buffer, sizeof( buffer ), data, args );
        va_end( args );
        sent += buffer;
    }
    void CloseConnection( SystemAddress ) override {}
    Packet* Receive( void ) override { return nullptr; }
    void DeallocatePacket( Packet* ) override {}
    SystemAddress HasNewIncomingConnection( void ) override { return UNASSIGNED_SYSTEM_ADDRESS; }
    SystemAddress HasLostConnection( void ) override { return UNASSIGNED_SYSTEM_ADDRESS; }
    CommandParserInterface* GetCommandParser( void ) override { return nullptr; }
};

} // namespace

TEST_CASE( "CommandParserInterface looks commands up by name without regard to case", "[commandparser]" )
{
    TestParser parser;
    parser.RegisterCommand( 1, "beta", "beta help" );
    parser.RegisterCommand( 0, "Alpha", "alpha help" );

    RegisteredCommand rc;
    REQUIRE( parser.GetRegisteredCommand( "ALPHA", &rc ) );
    CHECK( std::string( rc.command ) == "Alpha" );
    REQUIRE( parser.GetRegisteredCommand( "Beta", &rc ) );
    CHECK( std::string( rc.commandHelp ) == "beta help" );
    CHECK( rc.parameterCount == 1 );
    CHECK_FALSE( parser.GetRegisteredCommand( "gamma", &rc ) );
}

TEST_CASE( "CommandParserInterface lists its commands in case-insensitive name order", "[commandparser]" )
{
    TestParser parser;
    parser.RegisterCommand( 0, "gamma", "" );
    parser.RegisterCommand( 0, "Beta", "" );
    parser.RegisterCommand( 0, "alpha", "" );

    RecordingTransport transport;
    parser.SendCommandList( &transport, UNASSIGNED_SYSTEM_ADDRESS );

    CHECK( transport.sent == "alpha, Beta, gamma\r\n" );
}
