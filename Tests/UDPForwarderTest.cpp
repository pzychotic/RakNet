#include "Plugins/UDPForwarder.h"

#include "RakNetTypes.h"

#include <catch2/catch_test_macros.hpp>

using namespace RakNet;

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
