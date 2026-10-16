#include "RakNetStringMakers.h"
#include "RakNetTypes.h"
#include "WinsockScope.h"

#include <catch2/catch_test_macros.hpp>

/*
NonNumericHostString decides whether SystemAddress parses a host string as a numeric
address or hands it to name resolution. Hex digits are numeric in either case, so an IPv6
literal written in uppercase is parsed, not looked up.
*/

using namespace RakNet;

TEST_CASE( "NonNumericHostString classes IP literals in either case as numeric", "[address]" )
{
    CHECK_FALSE( NonNumericHostString( "FE80::1" ) );
    CHECK_FALSE( NonNumericHostString( "fe80::1" ) );
    CHECK_FALSE( NonNumericHostString( "2001:DB8:aBcD::Ef" ) );
    CHECK_FALSE( NonNumericHostString( "192.168.0.1" ) );

    CHECK( NonNumericHostString( "Example.com" ) );
    CHECK( NonNumericHostString( "localhost" ) );
    CHECK( NonNumericHostString( "G" ) );
}

TEST_CASE( "SystemAddress::FromString splits the port off an uppercase IPv6 literal", "[address]" )
{
#if RAKNET_SUPPORT_IPV6 == 1
    WinsockScope winsock;

    // As a hostname, the whole string including "|1234" would go to name resolution.
    SystemAddress upper;
    REQUIRE( upper.FromString( "FE80::1|1234", '|', 6 ) );
    SystemAddress lower;
    REQUIRE( lower.FromString( "fe80::1|1234", '|', 6 ) );

    CHECK( upper.GetPort() == 1234 );
    CHECK( upper == lower );
#else
    SKIP( "RAKNET_SUPPORT_IPV6 is 0, so SystemAddress has no AF_INET6 member." );
#endif
}
