#include "Plugins/UDPForwarder.h"

#include "ConnectionWaits.h"
#include "GetTime.h"
#include "RakNetTypes.h"

#include <catch2/catch_test_macros.hpp>

using namespace RakNet;

namespace {

// Hang guard for the forwarder thread to drop an entry. It takes one pass of its loop, or
// the entry's idle timeout.
constexpr TimeMS kDropBudgetMs = 5000;

// Polls the count from this thread, as Router2 does, until it reads expected.
bool CountReaches( const UDPForwarder& forwarder, int expected )
{
    const TimeMS deadline = GetTimeMS() + kDropBudgetMs;
    while( forwarder.GetUsedForwardEntries() != expected )
    {
        if( ConnectionWaits::Expired( deadline ) )
            return false;
    }
    return true;
}

} // namespace

TEST_CASE( "UDPForwarder counts its entries while its thread drops a stopped and an idle one", "[udpforwarder]" )
{
    constexpr TimeMS kIdleTimeoutMs = 200;

    UDPForwarder forwarder;
    forwarder.Startup();

    const SystemAddress source( "127.0.0.1", 31100 );
    const SystemAddress stopped( "127.0.0.1", 31101 );
    const SystemAddress idle( "127.0.0.1", 31102 );
    unsigned short port = 0;
    __UDPSOCKET__ socket = INVALID_SOCKET;
    REQUIRE( forwarder.StartForwarding( source, stopped, 30000, nullptr, AF_INET, &port, &socket ) == UDPFORWARDER_SUCCESS );
    REQUIRE( forwarder.StartForwarding( source, idle, kIdleTimeoutMs, nullptr, AF_INET, &port, &socket ) == UDPFORWARDER_SUCCESS );
    CHECK( forwarder.GetUsedForwardEntries() == 2 );

    forwarder.StopForwarding( source, stopped );
    CHECK( CountReaches( forwarder, 1 ) );
    CHECK( CountReaches( forwarder, 0 ) );

    forwarder.Shutdown();
}

TEST_CASE( "UDPForwarder hands back the existing port and socket for a pair it already forwards", "[udpforwarder]" )
{
    UDPForwarder forwarder;
    forwarder.Startup();

    const SystemAddress source( "127.0.0.1", 31100 );
    const SystemAddress destination( "127.0.0.1", 31101 );

    unsigned short firstPort = 0;
    __UDPSOCKET__ firstSocket = INVALID_SOCKET;
    REQUIRE( forwarder.StartForwarding( source, destination, 30000, nullptr, AF_INET, &firstPort, &firstSocket ) == UDPFORWARDER_SUCCESS );
    REQUIRE( firstPort != 0 );

    unsigned short againPort = 0;
    __UDPSOCKET__ againSocket = INVALID_SOCKET;
    CHECK( forwarder.StartForwarding( source, destination, 30000, nullptr, AF_INET, &againPort, &againSocket ) == UDPFORWARDER_FORWARDING_ALREADY_EXISTS );
    CHECK( againPort == firstPort );
    CHECK( againSocket == firstSocket );

    unsigned short reversePort = 0;
    __UDPSOCKET__ reverseSocket = INVALID_SOCKET;
    CHECK( forwarder.StartForwarding( destination, source, 30000, nullptr, AF_INET, &reversePort, &reverseSocket ) == UDPFORWARDER_FORWARDING_ALREADY_EXISTS );
    CHECK( reversePort == firstPort );
    CHECK( reverseSocket == firstSocket );

    forwarder.Shutdown();
}
