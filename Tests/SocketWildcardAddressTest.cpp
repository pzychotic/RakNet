#include "RakNetDefines.h"
#include "RakNetSocket2.h"
#include "RakNetTypes.h"
#include "SocketDefines.h"
#include "WSAStartupSingleton.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>

/*
Pins which bound addresses GetBoundAddress rewrites to loopback: the wildcard, and
nothing else.

RNS2_Berkley::GetSystemAddressIPV4And6 (and its twin, SocketLayer::GetSystemAddress)
reads the bound address back with getsockname and reports a wildcard bind as loopback.
The defect: the IPv6 branch compared the 16
bytes at &address.addr4.sin_addr against zero. That is offset 4 of the union, which in a
sockaddr_in6 is sin6_flowinfo followed by only the first 12 bytes of sin6_addr. `::`
still matched, but so did every address whose first 12 bytes are zero - the
IPv4-compatible ::a.b.c.d form - and those came back as ::1. A nonzero flow info also
hid a real `::` from it.

The discriminating cases are on IsWildcardAddress, with constructed addresses. A real
socket would be better, but Windows refuses to bind ::a.b.c.d at all, and those are the
only bindable addresses that tell the two checks apart. ::1 has zero leading bytes too,
so the unfixed check calls it a wildcard - but the rewrite turns it into ::1, so a
socket bound to it looks right either way. The bound-socket cases below still run the
real getsockname path, and the ::127.0.0.2 one skips where the host cannot bind it.
*/

using namespace RakNet;

namespace {

SystemAddress IPv4Address( const char* address )
{
    SystemAddress systemAddress = UNASSIGNED_SYSTEM_ADDRESS;
    systemAddress.address.addr4.sin_family = AF_INET;
    REQUIRE( inet_pton( AF_INET, address, &systemAddress.address.addr4.sin_addr ) == 1 );
    return systemAddress;
}

} // namespace

TEST_CASE( "IsWildcardAddress recognises INADDR_ANY and nothing else", "[socket]" )
{
    CHECK( IsWildcardAddress( IPv4Address( "0.0.0.0" ) ) );
    CHECK_FALSE( IsWildcardAddress( IPv4Address( "127.0.0.1" ) ) );
    CHECK_FALSE( IsWildcardAddress( IPv4Address( "0.0.0.1" ) ) );
}

#if RAKNET_SUPPORT_IPV6 == 1
namespace {

// Nothing here goes through RakPeer::Startup, so these take the same Winsock refcount
// RakNet itself does. A no-op off Windows.
struct WinsockFixture
{
    WinsockFixture() { WSAStartupSingleton::AddRef(); }
    ~WinsockFixture() { WSAStartupSingleton::Deref(); }
};

SystemAddress IPv6Address( const char* address, unsigned int flowInfo = 0 )
{
    SystemAddress systemAddress = UNASSIGNED_SYSTEM_ADDRESS;
    memset( &systemAddress.address.addr6, 0, sizeof( systemAddress.address.addr6 ) );
    systemAddress.address.addr6.sin6_family = AF_INET6;
    systemAddress.address.addr6.sin6_flowinfo = htonl( flowInfo );
    REQUIRE( inet_pton( AF_INET6, address, &systemAddress.address.addr6.sin6_addr ) == 1 );
    return systemAddress;
}

bool HoldsIPv6Address( const SystemAddress& systemAddress, const char* expected )
{
    in6_addr expectedAddress;
    memset( &expectedAddress, 0, sizeof( expectedAddress ) );
    if( inet_pton( AF_INET6, expected, &expectedAddress ) != 1 )
        return false;
    return memcmp( &systemAddress.address.addr6.sin6_addr, &expectedAddress, sizeof( expectedAddress ) ) == 0;
}

RNS2BindResult BindIPv6( RNS2_Berkley& berkleySocket, char* hostAddress )
{
    RNS2_BerkleyBindParameters bindParameters;
    memset( &bindParameters, 0, sizeof( bindParameters ) );

    // Port 0 lets the OS pick.
    bindParameters.port = 0;
    bindParameters.hostAddress = hostAddress;
    bindParameters.addressFamily = AF_INET6;
    bindParameters.type = SOCK_DGRAM;
    bindParameters.nonBlockingSocket = false;

    // Only the receive polling thread reads this, and no test here starts one.
    bindParameters.eventHandler = 0;

    return berkleySocket.Bind( &bindParameters, _FILE_AND_LINE_ );
}

} // namespace
#endif

TEST_CASE( "IsWildcardAddress recognises :: and nothing else", "[socket]" )
{
#if RAKNET_SUPPORT_IPV6 == 1
    CHECK( IsWildcardAddress( IPv6Address( "::" ) ) );

    // The flow info is not part of the address. The unfixed check read it as the first
    // four of its sixteen bytes.
    CHECK( IsWildcardAddress( IPv6Address( "::", 0x12345 ) ) );

    // First 12 bytes zero: the unfixed check stopped reading before the part that differs.
    CHECK_FALSE( IsWildcardAddress( IPv6Address( "::127.0.0.2" ) ) );
    CHECK_FALSE( IsWildcardAddress( IPv6Address( "::1" ) ) );

    CHECK_FALSE( IsWildcardAddress( IPv6Address( "2001:db8::1" ) ) );
#else
    SKIP( "RAKNET_SUPPORT_IPV6 is 0, so SystemAddress has no AF_INET6 member." );
#endif
}

TEST_CASE( "An IPv6 socket bound to :: reports loopback", "[socket]" )
{
#if RAKNET_SUPPORT_IPV6 == 1
    WinsockFixture winsock;

    RNS2_Berkley berkleySocket;
    char hostAddress[] = "::";
    if( BindIPv6( berkleySocket, hostAddress ) != BR_SUCCESS )
    {
        // A host with IPv6 switched off is not a failing library.
        SKIP( "No IPv6 available on this host." );
    }

    const SystemAddress boundAddress = berkleySocket.GetBoundAddress();
    CHECK( boundAddress.GetIPVersion() == 6 );
    CHECK( HoldsIPv6Address( boundAddress, "::1" ) );
#else
    SKIP( "RAKNET_SUPPORT_IPV6 is 0, so this build cannot bind an AF_INET6 socket." );
#endif
}

TEST_CASE( "An IPv6 socket bound to ::127.0.0.2 reports that address", "[socket]" )
{
#if RAKNET_SUPPORT_IPV6 == 1
    WinsockFixture winsock;

    RNS2_Berkley berkleySocket;
    char hostAddress[] = "::127.0.0.2";
    if( BindIPv6( berkleySocket, hostAddress ) != BR_SUCCESS )
    {
        // Windows, for one. IsWildcardAddress's case above covers this address instead.
        SKIP( "This host cannot bind ::127.0.0.2." );
    }

    const SystemAddress boundAddress = berkleySocket.GetBoundAddress();
    CHECK( boundAddress.GetIPVersion() == 6 );
    CHECK( HoldsIPv6Address( boundAddress, "::127.0.0.2" ) );
#else
    SKIP( "RAKNET_SUPPORT_IPV6 is 0, so this build cannot bind an AF_INET6 socket." );
#endif
}
