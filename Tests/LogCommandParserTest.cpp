#include "LogCommandParser.h"
#include "TransportInterface.h"

#include "RakNetTypes.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdarg>
#include <cstdio>
#include <string>

/*
LogCommandParser finds a channel by name without regard to case, among the channels added so
far, and a log written to a channel reaches only the remote users subscribed to it.
*/

using namespace RakNet;

namespace {

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

void SubscribeTo( LogCommandParser& parser, RecordingTransport& transport, const SystemAddress& user, const char* channelName )
{
    char parameter[32];
    snprintf( parameter, sizeof( parameter ), "%s", channelName );
    char* parameterList[] = { parameter };
    parser.OnCommand( "Subscribe", 1, parameterList, &transport, user, "" );
}

} // namespace

TEST_CASE( "LogCommandParser finds an added channel by name without regard to case", "[logcommandparser]" )
{
    LogCommandParser parser;
    parser.AddChannel( "Alpha" );
    parser.AddChannel( "Beta" );

    RecordingTransport transport;
    const SystemAddress user( "127.0.0.1", 1 );
    SubscribeTo( parser, transport, user, "bETA" );
    CHECK( transport.sent == "You are now subscribed to channel Beta.\r\n" );

    transport.sent.clear();
    SubscribeTo( parser, transport, user, "Gamma" );
    CHECK( transport.sent.rfind( "Cannot find channel Gamma.\r\n", 0 ) == 0 );
}

TEST_CASE( "LogCommandParser sends a log only to the users subscribed to its channel", "[logcommandparser]" )
{
    LogCommandParser parser;
    parser.AddChannel( "Alpha" );
    parser.AddChannel( "Beta" );

    RecordingTransport transport;
    parser.OnTransportChange( &transport );
    SubscribeTo( parser, transport, SystemAddress( "127.0.0.1", 1 ), "Beta" );

    transport.sent.clear();
    parser.WriteLog( "alpha", "to alpha" );
    CHECK( transport.sent.empty() );

    parser.WriteLog( "BETA", "to beta" );
    CHECK( transport.sent == "to beta\r\n" );
}

TEST_CASE( "LogCommandParser unsubscribes a user from a channel named without regard to case", "[logcommandparser]" )
{
    LogCommandParser parser;
    parser.AddChannel( "Alpha" );
    parser.AddChannel( "Beta" );

    RecordingTransport transport;
    parser.OnTransportChange( &transport );
    const SystemAddress user( "127.0.0.1", 1 );
    SubscribeTo( parser, transport, user, "Alpha" );
    SubscribeTo( parser, transport, user, "Beta" );

    transport.sent.clear();
    char channelName[] = "bEtA";
    char* parameterList[] = { channelName };
    parser.OnCommand( "Unsubscribe", 1, parameterList, &transport, user, "" );
    CHECK( transport.sent == "You are now unsubscribed from channel Beta.\r\n" );

    transport.sent.clear();
    parser.WriteLog( "Beta", "to beta" );
    parser.WriteLog( "Alpha", "to alpha" );
    CHECK( transport.sent == "to alpha\r\n" );
}

TEST_CASE( "LogCommandParser subscribes a user to every channel with no channel named", "[logcommandparser]" )
{
    LogCommandParser parser;
    parser.AddChannel( "Alpha" );
    parser.AddChannel( "Beta" );

    RecordingTransport transport;
    parser.OnTransportChange( &transport );
    parser.OnCommand( "Subscribe", 0, nullptr, &transport, SystemAddress( "127.0.0.1", 1 ), "" );
    CHECK( transport.sent == "Subscribed to all channels.\r\n" );

    transport.sent.clear();
    parser.WriteLog( "Alpha", "to alpha" );
    parser.WriteLog( "Beta", "to beta" );
    CHECK( transport.sent == "to alpha\r\nto beta\r\n" );
}
