#include "ConnectionWaits.h"
#include "InspectableTelnetTransport.h"
#include "LoopbackTCP.h"
#include "RakNetTypes.h"
#include "TCPInterface.h"
#include "WinsockScope.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>

/*
Pins how TelnetTransport accounts for connections that share an address: a client
reconnecting from the address it last connected from, and a connection whose lost event is
drained before its new one.

TelnetTransport keeps a TelnetClient per remote address for line reassembly, and TCPInterface
gives it nothing better to key on. New and lost events come from two separate queues, so
their relative order is lost, and a reconnect accepted after the old connection's loss was
detected usually takes the same slot, so even systemIndex repeats. What does hold is that
every connection reported new is reported lost once. So each entry counts the connections
still open at its address - new events drained minus lost events drained - and goes when
that reaches zero.

Three defects came before that:

- HasNewIncomingConnection pushed a reused entry onto remoteClients a second time, and
  HasLostConnection then freed it twice.
- The old connection's lost event freed the entry the reconnect was using, and Receive
  dropped the live connection's input from then on.
- A lost event drained before its new one found no entry and freed nothing, and the new
  event then created an entry that nothing would free.

An address repeats only when the source port does, so the clients here are raw sockets bound
to fixed local ports. They close abortively - SO_LINGER with a zero timeout sends a RST -
which leaves no TIME_WAIT behind and lets a reconnect bind the same port again.
*/

using namespace RakNet;

namespace {

// TCP, so these share no space with the UDP ports the rest of the suite hardcodes, and
// distinct from the TCPInterface tests' ports so a stray listener is never ambiguous about
// which test left it. CreateListenSocket does not set SO_REUSEADDR before it binds, so no
// port is shared between cases.
constexpr unsigned short kReconnectListenPort = 31030;
constexpr unsigned short kReconnectClientPort = 31031;
constexpr unsigned short kLostFirstListenPort = 31032;
constexpr unsigned short kLostFirstClientPort = 31033;

} // namespace

TEST_CASE( "TelnetTransport keeps a client reconnecting from the same address", "[telnettransport][network]" )
{
    WinsockScope winsock;

    InspectableTelnetTransport telnet;
    REQUIRE( telnet.Start( kReconnectListenPort, true ) );

    SystemAddress firstAddress = UNASSIGNED_SYSTEM_ADDRESS;
    LoopbackTCP::Client first( kReconnectListenPort, kReconnectClientPort );
    REQUIRE( ConnectionWaits::WaitUntil( [&] { return ( firstAddress = telnet.HasNewIncomingConnection() ) != UNASSIGNED_SYSTEM_ADDRESS; }, LoopbackTCP::kWaitBudget ) );
    REQUIRE( telnet.ClientCount() == 1 );

    first.Abort();

    // The lost event for the first connection is queued, or soon will be; it is deliberately
    // not drained until the reconnect's new event has been.
    SystemAddress secondAddress = UNASSIGNED_SYSTEM_ADDRESS;
    LoopbackTCP::Client second( kReconnectListenPort, kReconnectClientPort );
    REQUIRE( ConnectionWaits::WaitUntil( [&] { return ( secondAddress = telnet.HasNewIncomingConnection() ) != UNASSIGNED_SYSTEM_ADDRESS; }, LoopbackTCP::kWaitBudget ) );
    REQUIRE( secondAddress == firstAddress );

    // The reused TelnetClient is already listed, so it is not listed again.
    CHECK( telnet.ClientCount() == 1 );

    // The first connection's lost event. It frees the TelnetClient once at most, and - the
    // second connection being open at the same address - not at all.
    REQUIRE( ConnectionWaits::WaitUntil( [&] { return telnet.HasLostConnection() == firstAddress; }, LoopbackTCP::kWaitBudget ) );
    CHECK( telnet.ClientCount() == 1 );

    // The live connection's input still reassembles into a line.
    const char line[] = "hello\n";
    second.SendAll( line, strlen( line ) );
    Packet* received = 0;
    CHECK( ConnectionWaits::WaitUntil( [&] { return ( received = telnet.Receive() ) != 0; }, LoopbackTCP::kWaitBudget ) );
    if( received )
    {
        CHECK( received->systemAddress == secondAddress );
        CHECK( strcmp( (const char*)received->data, "hello" ) == 0 );
        telnet.DeallocatePacket( received );
    }

    // The second connection's own lost event frees it.
    second.Abort();
    CHECK( ConnectionWaits::WaitUntil( [&] { return telnet.HasLostConnection() == secondAddress; }, LoopbackTCP::kWaitBudget ) );
    CHECK( telnet.ClientCount() == 0 );

    telnet.Stop();
}

TEST_CASE( "TelnetTransport frees nothing twice and keeps nothing when a lost event comes first", "[telnettransport][network]" )
{
    WinsockScope winsock;

    InspectableTelnetTransport telnet;
    REQUIRE( telnet.Start( kLostFirstListenPort, true ) );

    SystemAddress address;
    REQUIRE( address.FromStringExplicitPort( "127.0.0.1", kLostFirstClientPort ) );

    // Accepted, but its new event left in the queue. Aborting before the accept could lose
    // the connection before TCPInterface ever reports it.
    LoopbackTCP::Client client( kLostFirstListenPort, kLostFirstClientPort );
    REQUIRE( ConnectionWaits::WaitUntil( [&] { return telnet.ConnectionCount() == 1; }, LoopbackTCP::kWaitBudget ) );
    client.Abort();

    // What ConsoleServer::Update does to a short-lived connection queued behind another: it
    // takes the lost event on one tick and the new event on a later one.
    REQUIRE( ConnectionWaits::WaitUntil( [&] { return telnet.HasLostConnection() == address; }, LoopbackTCP::kWaitBudget ) );
    REQUIRE( telnet.HasNewIncomingConnection() == address );

    // Unfixed, the lost event found no entry and the new one created an entry nothing frees.
    CHECK( telnet.ClientCount() == 0 );

    telnet.Stop();
}
