#include "RakNetStringMakers.h"
#include "RakNetTypes.h"
#include "WinsockScope.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <string>

/*
SystemAddress::FromString and RakNetGUID::FromString, the public parsers for an address
with an optional port and for a GUID written as a decimal number. No sockets: the
numeric hosts below never reach name resolution. FromString reaches inet_addr or
getaddrinfo, which Winsock documents as requiring WSAStartup, so each test holds a
WinsockScope.
*/

using namespace RakNet;

namespace {

std::string IpOf( const SystemAddress& address )
{
    char dest[64] = {};
    address.ToString( false, dest );
    return dest;
}

} // namespace

TEST_CASE( "SystemAddress::FromString reads the port after the delineator", "[address]" )
{
    WinsockScope winsock;

    SystemAddress address;
    REQUIRE( address.FromString( "192.168.1.5|1234" ) );
    CHECK( IpOf( address ) == "192.168.1.5" );
    CHECK( address.GetPort() == 1234 );

    REQUIRE( address.FromString( "10.0.0.1:80", ':' ) );
    CHECK( IpOf( address ) == "10.0.0.1" );
    CHECK( address.GetPort() == 80 );
}

TEST_CASE( "SystemAddress::FromString reads the ends of the port range", "[address]" )
{
    WinsockScope winsock;

    SystemAddress address( "192.168.1.5", 1234 );
    REQUIRE( address.FromString( "192.168.1.5|0" ) );
    CHECK( address.GetPort() == 0 );

    REQUIRE( address.FromString( "192.168.1.5|65535" ) );
    CHECK( address.GetPort() == 65535 );
}

TEST_CASE( "SystemAddress::FromString keeps the port when the string has none", "[address]" )
{
    WinsockScope winsock;

    SystemAddress address( "192.168.1.5", 1234 );
    REQUIRE( address.FromString( "10.0.0.1" ) );
    CHECK( IpOf( address ) == "10.0.0.1" );
    CHECK( address.GetPort() == 1234 );

    REQUIRE( address.FromString( "10.0.0.2|" ) );
    CHECK( IpOf( address ) == "10.0.0.2" );
    CHECK( address.GetPort() == 1234 );
}

TEST_CASE( "The SystemAddress string constructor reads the port", "[address]" )
{
    WinsockScope winsock;

    const SystemAddress address( "192.168.1.5|4321" );
    CHECK( IpOf( address ) == "192.168.1.5" );
    CHECK( address.GetPort() == 4321 );
}

TEST_CASE( "SystemAddress::FromString rejects a port that is not a whole number up to 65535", "[address]" )
{
    WinsockScope winsock;

    const char* const rejected[] = {
        "10.0.0.1|65536",
        "10.0.0.1|99999",
        "10.0.0.1|4294967376",
        "10.0.0.1| 80",
        "10.0.0.1|+80",
        "10.0.0.1|-1",
        "10.0.0.1|8.0",
    };
    for( const char* text : rejected )
    {
        CAPTURE( text );
        SystemAddress address( "192.168.1.5", 1234 );
        CHECK_FALSE( address.FromString( text ) );
        CHECK( IpOf( address ) == "192.168.1.5" );
        CHECK( address.GetPort() == 1234 );
    }

    // A letter anywhere makes the string a host name, which then fails to resolve.
    SystemAddress address;
    CHECK_FALSE( address.FromString( "10.0.0.1|80x" ) );
}

TEST_CASE( "SystemAddress::SetBinaryAddress reads the port after localhost", "[address]" )
{
    WinsockScope winsock;

    SystemAddress address( "192.168.1.5", 1234 );
    REQUIRE( address.SetBinaryAddress( "localhost:4321" ) );
    CHECK( IpOf( address ) == "127.0.0.1" );
    CHECK( address.GetPort() == 4321 );

    REQUIRE( address.SetBinaryAddress( "LOCALHOST" ) );
    CHECK( IpOf( address ) == "127.0.0.1" );
    CHECK( address.GetPort() == 4321 );

    SystemAddress rejected( "192.168.1.5", 1234 );
    CHECK_FALSE( rejected.SetBinaryAddress( "localhost:70000" ) );
    CHECK( IpOf( rejected ) == "192.168.1.5" );
    CHECK( rejected.GetPort() == 1234 );
}

TEST_CASE( "SystemAddress::SetBinaryAddress rejects a numeric host longer than 21 characters", "[address]" )
{
    WinsockScope winsock;

    SystemAddress address( "192.168.1.5", 1234 );
    CHECK_FALSE( address.SetBinaryAddress( "1111111111111111111111|80" ) );
    CHECK( IpOf( address ) == "192.168.1.5" );
    CHECK( address.GetPort() == 1234 );
}

TEST_CASE( "SystemAddress::SetBinaryAddress rejects a numeric host ended by anything but the delineator", "[address]" )
{
    WinsockScope winsock;

    const char* const rejected[] = {
        "1.2.3.4 80",
        "1.2.3.4/24",
        "1.2.3.4 ",
        " 80",
    };
    for( const char* text : rejected )
    {
        CAPTURE( text );
        SystemAddress address( "192.168.1.5", 1234 );
        CHECK_FALSE( address.SetBinaryAddress( text, '|' ) );
        CHECK( IpOf( address ) == "192.168.1.5" );
        CHECK( address.GetPort() == 1234 );
    }
}

TEST_CASE( "SystemAddress::SetBinaryAddress sets only the port for an empty host", "[address]" )
{
    WinsockScope winsock;

    SystemAddress address( "192.168.1.5", 1234 );
    REQUIRE( address.SetBinaryAddress( "|80", '|' ) );
    CHECK( IpOf( address ) == "192.168.1.5" );
    CHECK( address.GetPort() == 80 );
}

TEST_CASE( "SystemAddress::FromString reads the port after a host name", "[address]" )
{
    WinsockScope winsock;

    // The trailing dot resolves from the hosts file and skips SetBinaryAddress's localhost
    // shortcut.
    SystemAddress address( "192.168.1.5", 1234 );
    REQUIRE( address.FromString( "localhost.|80" ) );
    CHECK( address.IsLoopback() );
    CHECK( address.GetPort() == 80 );
}

TEST_CASE( "SystemAddress::FromString rejects a host name longer than 254 characters", "[address]" )
{
    WinsockScope winsock;

    std::string text;
    for( int label = 0; label < 4; label++ )
        text += std::string( 62, 'g' ) + ".";
    text += "ggg";
    REQUIRE( text.size() == 255 );

    SystemAddress address( "192.168.1.5", 1234 );
    CHECK_FALSE( address.FromString( text.c_str() ) );
    CHECK( IpOf( address ) == "192.168.1.5" );
    CHECK( address.GetPort() == 1234 );
}

#if RAKNET_SUPPORT_IPV6 == 1
TEST_CASE( "SystemAddress::FromString does not cut a host name short", "[address]" )
{
    WinsockScope winsock;

    // Cut at 56 characters, this would be the octal literal for 127.0.0.1.
    const std::string text = std::string( 46, '0' ) + "0177.0.0.1" + ".g";
    SystemAddress address( "192.168.1.5", 1234 );
    CHECK_FALSE( address.FromString( text.c_str() ) );
    CHECK( IpOf( address ) == "192.168.1.5" );
    CHECK( address.GetPort() == 1234 );
}

TEST_CASE( "SystemAddress::FromString rejects a numeric host ended by anything but the delineator", "[address]" )
{
    WinsockScope winsock;

    for( const char* text : { "1.2.3.4 80", "1.2.3.4 " } )
    {
        CAPTURE( text );
        SystemAddress address( "192.168.1.5", 1234 );
        CHECK_FALSE( address.FromString( text ) );
        CHECK( IpOf( address ) == "192.168.1.5" );
        CHECK( address.GetPort() == 1234 );
    }
}
#else
TEST_CASE( "SystemAddress::FromString does not read host:port with the default delineator", "[address]" )
{
    WinsockScope winsock;

    SystemAddress address( "192.168.1.5", 1234 );
    CHECK_FALSE( address.FromString( "1.2.3.4:80" ) );
    CHECK( IpOf( address ) == "192.168.1.5" );
    CHECK( address.GetPort() == 1234 );
}
#endif

#if RAKNET_SUPPORT_IPV6 == 1
TEST_CASE( "SystemAddress::FromString rejects a numeric host longer than 56 characters", "[address]" )
{
    WinsockScope winsock;

    // A zone padded with zeros resolves at any length, so only the length rejects it.
    const std::string text = "2001:db8::1%" + std::string( 64, '0' ) + "1|80";
    SystemAddress address( "192.168.1.5", 1234 );
    CHECK_FALSE( address.FromString( text.c_str() ) );
    CHECK( IpOf( address ) == "192.168.1.5" );
    CHECK( address.GetPort() == 1234 );
}

TEST_CASE( "SystemAddress::FromString reads a zoned IPv6 literal whole", "[address]" )
{
    WinsockScope winsock;

    SystemAddress address;
    REQUIRE( address.FromString( "ffff:ffff:ffff:ffff:ffff:ffff:255.255.255.255%1|80" ) );
    REQUIRE( address.GetIPVersion() == 6 );
    const unsigned char allOnes[16] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    CHECK( std::memcmp( &address.address.addr6.sin6_addr, allOnes, sizeof( allOnes ) ) == 0 );
    CHECK( address.address.addr6.sin6_scope_id == 1 );
    CHECK( address.GetPort() == 80 );
}
#endif

TEST_CASE( "RakNetGUID::FromString reads a decimal GUID", "[guid]" )
{
    RakNetGUID guid;
    REQUIRE( guid.FromString( "0" ) );
    CHECK( guid.g == 0 );

    REQUIRE( guid.FromString( "1234567890123" ) );
    CHECK( guid.g == UINT64_C( 1234567890123 ) );

    REQUIRE( guid.FromString( "18446744073709551614" ) );
    CHECK( guid.g == UINT64_C( 18446744073709551614 ) );
}

TEST_CASE( "RakNetGUID::FromString rejects a null string", "[guid]" )
{
    RakNetGUID guid( 5 );
    CHECK_FALSE( guid.FromString( nullptr ) );
    CHECK( guid.g == 5 );
}

TEST_CASE( "RakNetGUID::FromString rejects anything but a whole decimal number below 2^64", "[guid]" )
{
    const char* const rejected[] = {
        "",
        " 123",
        "+123",
        "-1",
        "123abc",
        "0x10",
        "18446744073709551616",
        "abc",
    };
    for( const char* text : rejected )
    {
        CAPTURE( text );
        RakNetGUID guid( 5 );
        CHECK_FALSE( guid.FromString( text ) );
        CHECK( guid.g == 5 );
    }
}

TEST_CASE( "RakNetGUID::FromString reads the name ToString gives the unassigned GUID", "[guid]" )
{
    RakNetGUID guid( 5 );
    REQUIRE( guid.FromString( UNASSIGNED_RAKNET_GUID.ToString() ) );
    CHECK( guid == UNASSIGNED_RAKNET_GUID );
}

TEST_CASE( "RakNetGUID::ToString writes the GUID in decimal", "[guid]" )
{
    char dest[64] = {};
    RakNetGUID( 0 ).ToString( dest );
    CHECK( std::strcmp( dest, "0" ) == 0 );

    RakNetGUID( UINT64_C( 1234567890123 ) ).ToString( dest );
    CHECK( std::strcmp( dest, "1234567890123" ) == 0 );

    RakNetGUID( UINT64_C( 18446744073709551614 ) ).ToString( dest );
    CHECK( std::strcmp( dest, "18446744073709551614" ) == 0 );

    UNASSIGNED_RAKNET_GUID.ToString( dest );
    CHECK( std::strcmp( dest, "UNASSIGNED_RAKNET_GUID" ) == 0 );
}

TEST_CASE( "RakNetGUID::FromString reads back what ToString writes", "[guid]" )
{
    for( const uint64_t value : { UINT64_C( 0 ), UINT64_C( 1 ), UINT64_C( 9876543210 ), UINT64_C( 18446744073709551614 ) } )
    {
        const RakNetGUID original( value );
        RakNetGUID parsed;
        REQUIRE( parsed.FromString( original.ToString() ) );
        CHECK( parsed == original );
    }
}
