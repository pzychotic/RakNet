#include "ConnectionWaits.h"
#include "LoopbackTCP.h"
#include "Plugins/TelnetTransport.h"
#include "RakNetTypes.h"
#include "WinsockScope.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

/*
Pins that TelnetTransport::Send delivers the send prefix and suffix whole, at any length,
around the formatted text, and that the formatted text keeps all REMOTE_MAX_TEXT_INPUT - 1
characters whatever the prefix and suffix are.
*/

using namespace RakNet;

namespace {

// Ports no other test uses.
constexpr unsigned short kFullPrefixListenPort = 32800;
constexpr unsigned short kFullTextListenPort = 32801;
constexpr unsigned short kLongPrefixListenPort = 32802;
constexpr unsigned short kClearedByNullListenPort = 32803;
constexpr unsigned short kClearedByEmptyListenPort = 32804;

// What TelnetTransport sends a client as it connects: IAC WILL ECHO.
const std::string kWillEcho = "\xff\xfb\x01";

// The longest text Send formats.
constexpr size_t kMaxFormatted = REMOTE_MAX_TEXT_INPUT - 1;

// A started server-mode TelnetTransport with one raw client, whose connection is drained so
// Send has its address.
struct ConnectedTelnet
{
    explicit ConnectedTelnet( unsigned short listenPort )
    : started( telnet.Start( listenPort, true ) )
    , client( listenPort )
    {
        REQUIRE( started );
        REQUIRE( ConnectionWaits::WaitUntil( [&] { return ( address = telnet.HasNewIncomingConnection() ) != UNASSIGNED_SYSTEM_ADDRESS; }, LoopbackTCP::kWaitBudget ) );
        REQUIRE( client.ReceiveExactly( kWillEcho.size() ) == kWillEcho );
    }

    ~ConnectedTelnet()
    {
        client.Abort();
        telnet.Stop();
    }

    WinsockScope winsock;
    TelnetTransport telnet;
    // Start's result, taken before client connects and checked in the body.
    bool started;
    LoopbackTCP::Client client;
    SystemAddress address = UNASSIGNED_SYSTEM_ADDRESS;
};

} // namespace

TEST_CASE( "TelnetTransport::Send formats the text after a prefix that fills the buffer", "[telnettransport][network]" )
{
    ConnectedTelnet connected( kFullPrefixListenPort );
    const std::string prefix( kMaxFormatted, 'p' );
    connected.telnet.SetSendPrefix( prefix.c_str() );

    connected.telnet.Send( connected.address, "x" );

    CHECK( connected.client.ReceiveExactly( prefix.size() + 1 ) == prefix + "x" );
}

TEST_CASE( "TelnetTransport::Send keeps the suffix after text that fills the buffer", "[telnettransport][network]" )
{
    ConnectedTelnet connected( kFullTextListenPort );
    connected.telnet.SetSendSuffix( "\r\n" );
    const std::string message( kMaxFormatted, 'm' );

    connected.telnet.Send( connected.address, "%s", message.c_str() );

    CHECK( connected.client.ReceiveExactly( message.size() + 2 ) == message + "\r\n" );
}

TEST_CASE( "TelnetTransport::Send sends a prefix longer than the buffer whole", "[telnettransport][network]" )
{
    ConnectedTelnet connected( kLongPrefixListenPort );
    const std::string prefix( 2 * REMOTE_MAX_TEXT_INPUT, 'p' );
    connected.telnet.SetSendPrefix( prefix.c_str() );

    connected.telnet.Send( connected.address, "x" );

    CHECK( connected.client.ReceiveExactly( prefix.size() + 1 ) == prefix + "x" );
}

TEST_CASE( "TelnetTransport::Send sends only the text once the prefix and suffix are cleared", "[telnettransport][network]" )
{
    const char* cleared = nullptr;
    unsigned short listenPort = 0;
    SECTION( "by nullptr" )
    {
        cleared = nullptr;
        listenPort = kClearedByNullListenPort;
    }
    SECTION( "by an empty string" )
    {
        cleared = "";
        listenPort = kClearedByEmptyListenPort;
    }

    ConnectedTelnet connected( listenPort );
    connected.telnet.SetSendPrefix( "<" );
    connected.telnet.SetSendSuffix( ">" );
    connected.telnet.SetSendPrefix( cleared );
    connected.telnet.SetSendSuffix( cleared );

    connected.telnet.Send( connected.address, "%d", 42 );
    connected.telnet.Send( connected.address, "!" );

    // A prefix or suffix left behind would show between or around the two sends.
    CHECK( connected.client.ReceiveExactly( 3 ) == "42!" );
}
