#include "AddressString.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>

/*
ClassifyHost, which decides how SetBinaryAddress and FromString read the host of an
address string: as no address, a numeric host, a host name for the resolver, or malformed.
*/

using namespace RakNet;

namespace {

HostKind Classify( const std::string& host )
{
    return ClassifyHost( host.c_str(), host.size() );
}

void CheckAll( std::initializer_list<const char*> hosts, HostKind kind )
{
    for( const char* host : hosts )
    {
        CAPTURE( host );
        CHECK( Classify( host ) == kind );
    }
}

} // namespace

TEST_CASE( "ClassifyHost reads a name whose letters are all a-f as a host name", "[address]" )
{
    CheckAll( { "cafe.de", "bad.cafe", "dead.beef.ad", "ABBA" }, HostKind::HostName );
}

TEST_CASE( "ClassifyHost reads an empty host as empty", "[address]" )
{
    CHECK( Classify( "" ) == HostKind::Empty );
}

TEST_CASE( "ClassifyHost reads other names as host names", "[address]" )
{
    CheckAll( { "localhost", "localhost.", "example.com" }, HostKind::HostName );
}

TEST_CASE( "ClassifyHost reads only a strict dotted quad as IPv4", "[address]" )
{
    CheckAll( { "0.0.0.0", "1.2.3.4", "255.255.255.255" }, HostKind::IPv4 );
    CheckAll( { "1.2.3.4.5", "1.2.3", "1.2.3.256", "1.2.3.999", "01.2.3.4", "010.0.0.1", "0177.0.0.1",
                "0x7f.0.0.1", "127.1", "1.2.3.4.", "1..2.3", "-1.2.3.4", "4294967296", "1.2.3.99999999999999999999" },
              HostKind::Malformed );
}

TEST_CASE( "ClassifyHost reads a host whose last label is not all digits as a host name", "[address]" )
{
    CheckAll( { "1.2.3.0x4", "1.2.3.4x" }, HostKind::HostName );
}

TEST_CASE( "ClassifyHost reads an address inet_pton accepts as IPv6", "[address]" )
{
    CheckAll( { "::1", "fe80::1", "FE80::1", "2001:DB8:aBcD::Ef", "::ffff:1.2.3.4" }, HostKind::IPv6 );
    CheckAll( { "::zz", "1:2:3:4:5:6:7:8:9", "[::1]", "fe80::1%" }, HostKind::Malformed );
}

TEST_CASE( "ClassifyHost reads a zone of up to 15 characters after an IPv6 address", "[address]" )
{
    CheckAll( { "fe80::1%14", "fe80::1%eth0", "fe80::1%en0.100_a-b" }, HostKind::IPv6 );
    CHECK( Classify( "fe80::1%" + std::string( 15, '1' ) ) == HostKind::IPv6 );
    CHECK( Classify( "fe80::1%" + std::string( 16, '1' ) ) == HostKind::Malformed );
    CheckAll( { "fe80::1%eth 0", "fe80::1%1%2", "fe80::1%eth0/1" }, HostKind::Malformed );

    const std::string longest = "ffff:ffff:ffff:ffff:ffff:ffff:255.255.255.255%" + std::string( 15, 'z' );
    REQUIRE( longest.size() == MaxNumericHostLength );
    CHECK( Classify( longest ) == HostKind::IPv6 );
}

TEST_CASE( "ClassifyHost reads only the given length", "[address]" )
{
    const char text[] = "1.2.3.4|80";
    CHECK( ClassifyHost( text, strlen( "1.2.3.4" ) ) == HostKind::IPv4 );
}
