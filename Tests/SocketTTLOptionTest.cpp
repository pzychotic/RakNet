#include "RakNetDefines.h"
#include "RakNetSocket2.h"
#include "RakNetTypes.h"
#include "SocketDefines.h"
#include "WSAStartupSingleton.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>

/*
Pins the pairing of a socket option *level* with its option *name* in Send_NoVDP.

The defect: SystemAddress::GetIPPROTO returns IPPROTO_IP for AF_INET and IPPROTO_IPV6
for AF_INET6, and every TTL call passed that moving level with a hardcoded IP_TTL. An
option name only means something within its level, so for an AF_INET6 address the pair
was getsockopt( fd, IPPROTO_IPV6, IP_TTL, ... ). The IPv6 spelling of that option is
IPV6_UNICAST_HOPS.

Windows cannot tell the two apart. ws2ipdef.h gives IP_TTL and IPV6_UNICAST_HOPS both
the value 4 (and IP_DONTFRAGMENT and IPV6_DONTFRAG both 14), so the wrong name under
the right level still lands on the hop limit and the defect is invisible - it always
was, which is why it survived this long. On Linux IP_TTL is 2 and IPV6_UNICAST_HOPS is
16, and option 2 under IPPROTO_IPV6 is IPV6_2292PKTINFO: a different option that accepts
the read and the write without complaint, so the hop limit is silently never set.

That last part is why the end-to-end case below is not the load-bearing one. Saving,
clobbering and restoring the *wrong* option leaves the socket looking exactly as tidy
afterwards as saving and restoring the right one, so "the send succeeded and the hop
limit came back unchanged" holds under the defect too, on every platform. It is here
because the ticket asks for it and because it exercises the real call path on a real
bound socket, not because it can discriminate.

The case that can discriminate is the first one, on GetTTLOptionName itself: it names
the constant that must come out for each family, which is a fact no numeric coincidence
can paper over. It is the reason GetTTLOptionName is declared in RakNetSocket2.h rather
than kept file-static next to its call sites.
*/

using namespace RakNet;

namespace {

// Nothing here goes through RakPeer::Startup, so these take the same Winsock refcount
// RakNet itself does. A no-op off Windows.
struct WinsockFixture
{
    WinsockFixture() { WSAStartupSingleton::AddRef(); }
    ~WinsockFixture() { WSAStartupSingleton::Deref(); }
};

SystemAddress AddressOfFamily( int addressFamily )
{
    SystemAddress systemAddress = UNASSIGNED_SYSTEM_ADDRESS;

    // The family is the only input GetIPPROTO and GetTTLOptionName read. Both spellings
    // of the family field sit at offset 0 of the union and have the same type, so the
    // addr4 one reaches either.
    systemAddress.address.addr4.sin_family = (decltype( systemAddress.address.addr4.sin_family ))addressFamily;

    return systemAddress;
}

void FillBindParameters( RNS2_BerkleyBindParameters* bindParameters, unsigned short addressFamily, char* hostAddress )
{
    memset( bindParameters, 0, sizeof( RNS2_BerkleyBindParameters ) );

    // Port 0 lets the OS pick; Bind fills the assigned one into the bound address.
    bindParameters->port = 0;
    bindParameters->hostAddress = hostAddress;
    bindParameters->addressFamily = addressFamily;
    bindParameters->type = SOCK_DGRAM;
    bindParameters->protocol = 0;
    bindParameters->nonBlockingSocket = false;
    bindParameters->setBroadcast = 0;
    bindParameters->setIPHdrIncl = 0;
    bindParameters->doNotFragment = 0;
    bindParameters->pollingThreadPriority = 0;

    // Only the receive polling thread reads this, and no test here starts one.
    bindParameters->eventHandler = 0;
}

// Sends one datagram from socket to itself with the given ttl, and checks that the TTL
// option is back where it started afterwards.
//
// The level and name are the caller's, spelled out literally at each call site, rather
// than whatever GetTTLOptionName returns. Reading back through the function under test
// would make this self-referential: a helper that named the wrong option would save,
// clobber and restore that same wrong option, and the read-back would agree with it.
void CheckSendWithTTLRestoresOption( RNS2_Berkley& berkleySocket, int ttl, int level, int optionName )
{
    const SystemAddress boundAddress = berkleySocket.GetBoundAddress();

    int optionBefore = 0;
    socklen_t optionLength = sizeof( optionBefore );
    REQUIRE( getsockopt__( berkleySocket.GetSocket(), level, optionName, (char*)&optionBefore, &optionLength ) == 0 );

    char payload[8];
    memset( payload, 'x', sizeof( payload ) );

    RNS2_SendParameters sendParameters;
    sendParameters.data = payload;
    sendParameters.length = (int)sizeof( payload );
    sendParameters.systemAddress = boundAddress;
    sendParameters.ttl = ttl;

    CHECK( berkleySocket.Send( &sendParameters, _FILE_AND_LINE_ ) == (RNS2SendResult)sizeof( payload ) );

    int optionAfter = 0;
    optionLength = sizeof( optionAfter );
    REQUIRE( getsockopt__( berkleySocket.GetSocket(), level, optionName, (char*)&optionAfter, &optionLength ) == 0 );
    CHECK( optionAfter == optionBefore );
}

} // namespace

TEST_CASE( "The TTL option name follows the level GetIPPROTO returns", "[socket]" )
{
    const SystemAddress ipv4Address = AddressOfFamily( AF_INET );

    CHECK( ipv4Address.GetIPPROTO() == (unsigned int)IPPROTO_IP );
    CHECK( GetTTLOptionName( ipv4Address ) == IP_TTL );

    const SystemAddress ipv6Address = AddressOfFamily( AF_INET6 );

#if RAKNET_SUPPORT_IPV6 == 1
    // The whole defect in one pair of lines: the level moves to IPPROTO_IPV6, so the name
    // has to move to IPV6_UNICAST_HOPS with it.
    CHECK( ipv6Address.GetIPPROTO() == (unsigned int)IPPROTO_IPV6 );
    CHECK( GetTTLOptionName( ipv6Address ) == IPV6_UNICAST_HOPS );
#else
    // This build has no IPv6 support at all: GetIPPROTO returns IPPROTO_IP whatever the
    // family is, so the name must stay IP_TTL to match. This ticket is a no-op here, and
    // this is what says so.
    CHECK( ipv6Address.GetIPPROTO() == (unsigned int)IPPROTO_IP );
    CHECK( GetTTLOptionName( ipv6Address ) == IP_TTL );
#endif
}

TEST_CASE( "Sending with a TTL over IPv4 succeeds and restores the option", "[socket]" )
{
    WinsockFixture winsock;

    RNS2_Berkley berkleySocket;
    char hostAddress[] = "127.0.0.1";
    RNS2_BerkleyBindParameters bindParameters;
    FillBindParameters( &bindParameters, AF_INET, hostAddress );

    REQUIRE( berkleySocket.Bind( &bindParameters, _FILE_AND_LINE_ ) == BR_SUCCESS );

    // ttl 2 is what NatPunchthroughClient::SendTTL uses, by way of RakPeer::SendTTL - the
    // only caller in the tree that sets ttl at all.
    CheckSendWithTTLRestoresOption( berkleySocket, 2, IPPROTO_IP, IP_TTL );
}

TEST_CASE( "Sending with a TTL over IPv6 succeeds and restores the option", "[socket]" )
{
#if RAKNET_SUPPORT_IPV6 == 1
    WinsockFixture winsock;

    RNS2_Berkley berkleySocket;
    char hostAddress[] = "::1";
    RNS2_BerkleyBindParameters bindParameters;
    FillBindParameters( &bindParameters, AF_INET6, hostAddress );

    if( berkleySocket.Bind( &bindParameters, _FILE_AND_LINE_ ) != BR_SUCCESS )
    {
        // A host with IPv6 switched off is not a failing library.
        SKIP( "No IPv6 loopback available on this host." );
    }

    // Against the unfixed code this reaches getsockopt( fd, IPPROTO_IPV6, IP_TTL, ... ).
    // On Windows that is the hop limit anyway and the case passes either way; on a
    // platform where the numbers differ it addresses an unrelated option, which this
    // cannot see because that option is saved and restored just as neatly. See the file
    // comment: the discriminating assertion is in the GetTTLOptionName case above.
    CheckSendWithTTLRestoresOption( berkleySocket, 2, IPPROTO_IPV6, IPV6_UNICAST_HOPS );
#else
    SKIP( "RAKNET_SUPPORT_IPV6 is 0, so this build cannot send to an AF_INET6 address." );
#endif
}
