/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

/// \file
///


#include "RakNetTypes.h"
#include "RakAssert.h"
#include <string.h>
#include <stdio.h>
#include "WindowsIncludes.h"
#include "WSAStartupSingleton.h"
#include "SocketDefines.h"
#include "AddrInfo.h"
#include "EqualsIgnoringAsciiCase.h"


#if defined( _WIN32 )
// IP_DONTFRAGMENT is different between winsock 1 and winsock 2.  Therefore, Winsock2.h must be linked againt Ws2_32.lib
// winsock.h must be linked against WSock32.lib.  If these two are mixed up the flag won't work correctly
#include "WindowsIncludes.h"

#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#endif

#include "AddressString.h"
#include "ParseWholeNumber.h"
#include "SuperFastHash.h"

#include <algorithm>
#include <charconv>
#include <cinttypes>
#include <optional>
#include <stdlib.h>

namespace RakNet {

namespace {

// Reads portText, the text after a port delineator, as a port. Empty text is no port and
// leaves port empty. Returns false for text that isn't a whole number from 0 to 65535.
bool ReadPort( const char* portText, std::optional<unsigned short>& port )
{
    if( portText[0] == 0 )
        return true;
    unsigned short value = 0;
    if( ParseWholeNumber( portText, value ) == false )
        return false;
    port = value;
    return true;
}

// The longest host name read: a DNS name of 253 characters and a trailing dot.
constexpr size_t MaxHostNameLength = 253 + 1;

// The length of the host at the start of str: everything before the delineator, or all of
// str when the delineator is 0 or absent.
size_t HostLength( const char* str, char portDelineator )
{
    size_t length = 0;
    while( str[length] != 0 && str[length] != portDelineator )
        length++;
    return length;
}

// The first IPv4 address getaddrinfo lists for name, or none when the name has none.
std::optional<in_addr> ResolveIPv4( const char* name )
{
    WSAStartupSingleton::AddRef();
    addrinfo hints;
    memset( &hints, 0, sizeof hints );
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    std::optional<in_addr> ip;
    {
        // Freed before the Deref, which can be the WSACleanup.
        const AddrInfoPtr servinfo = ResolveAddrInfo( name, nullptr, hints );
        if( servinfo )
            ip = reinterpret_cast<const sockaddr_in*>( servinfo->ai_addr )->sin_addr;
    }
    WSAStartupSingleton::Deref();
    return ip;
}

} // namespace

const char* IPV6_LOOPBACK = "::1";
const char* IPV4_LOOPBACK = "127.0.0.1";

AddressOrGUID::AddressOrGUID( Packet* packet )
{
    rakNetGuid = packet->guid;
    systemAddress = packet->systemAddress;
}

unsigned long AddressOrGUID::ToInteger( const AddressOrGUID& aog )
{
    if( aog.rakNetGuid != UNASSIGNED_RAKNET_GUID )
        return RakNetGUID::ToUint32( aog.rakNetGuid );
    return SystemAddress::ToInteger( aog.systemAddress );
}
const char* AddressOrGUID::ToString( bool writePort ) const
{
    if( rakNetGuid != UNASSIGNED_RAKNET_GUID )
        return rakNetGuid.ToString();
    return systemAddress.ToString( writePort );
}
void AddressOrGUID::ToString( bool writePort, char* dest ) const
{
    if( rakNetGuid != UNASSIGNED_RAKNET_GUID )
        return rakNetGuid.ToString( dest );
    return systemAddress.ToString( writePort, dest );
}
SocketDescriptor::SocketDescriptor()
{
    port = 0;
    hostAddress[0] = 0;
    extraSocketOptions = 0;
    socketFamily = AF_INET;
}

SocketDescriptor::SocketDescriptor( unsigned short _port, const char* _hostAddress )
{
    port = _port;
    if( _hostAddress )
        strcpy( hostAddress, _hostAddress );
    else
        hostAddress[0] = 0;
    extraSocketOptions = 0;
    socketFamily = AF_INET;
}

// Defaults to not in peer to peer mode for NetworkIDs.  This only sends the localSystemAddress portion in the BitStream class
// This is what you want for client/server, where the server assigns all NetworkIDs and it is unnecessary to transmit the full structure.
// For peer to peer, this will transmit the systemAddress of the system that created the object in addition to localSystemAddress.  This allows
// Any system to create unique ids locally.
// All systems must use the same value for this variable.
//bool RAK_DLL_EXPORT NetworkID::peerToPeerMode=false;

SystemAddress& SystemAddress::operator=( const SystemAddress& input )
{
    memcpy( &address, &input.address, sizeof( address ) );
    systemIndex = input.systemIndex;
    debugPort = input.debugPort;
    return *this;
}
bool SystemAddress::EqualsExcludingPort( const SystemAddress& right ) const
{
    return ( address.addr4.sin_family == AF_INET && address.addr4.sin_addr.s_addr == right.address.addr4.sin_addr.s_addr )
#if RAKNET_SUPPORT_IPV6 == 1
           || ( address.addr4.sin_family == AF_INET6 && memcmp( address.addr6.sin6_addr.s6_addr, right.address.addr6.sin6_addr.s6_addr, sizeof( address.addr6.sin6_addr.s6_addr ) ) == 0 )
#endif
        ;
}
unsigned short SystemAddress::GetPort( void ) const
{
    return ntohs( address.addr4.sin_port );
}
unsigned short SystemAddress::GetPortNetworkOrder( void ) const
{
    return address.addr4.sin_port;
}
void SystemAddress::SetPortHostOrder( unsigned short s )
{
    address.addr4.sin_port = htons( s );
    debugPort = s;
}
void SystemAddress::SetPortNetworkOrder( unsigned short s )
{
    address.addr4.sin_port = s;
    debugPort = ntohs( s );
}
bool SystemAddress::operator==( const SystemAddress& right ) const
{
    return address.addr4.sin_port == right.address.addr4.sin_port && EqualsExcludingPort( right );
}

bool SystemAddress::operator!=( const SystemAddress& right ) const
{
    return ( *this == right ) == false;
}

bool SystemAddress::operator>( const SystemAddress& right ) const
{
    if( address.addr4.sin_port == right.address.addr4.sin_port )
    {
#if RAKNET_SUPPORT_IPV6 == 1
        if( address.addr4.sin_family == AF_INET )
            return address.addr4.sin_addr.s_addr > right.address.addr4.sin_addr.s_addr;
        return memcmp( address.addr6.sin6_addr.s6_addr, right.address.addr6.sin6_addr.s6_addr, sizeof( address.addr6.sin6_addr.s6_addr ) ) > 0;
#else
        return address.addr4.sin_addr.s_addr > right.address.addr4.sin_addr.s_addr;
#endif
    }
    return address.addr4.sin_port > right.address.addr4.sin_port;
}

bool SystemAddress::operator<( const SystemAddress& right ) const
{
    if( address.addr4.sin_port == right.address.addr4.sin_port )
    {
#if RAKNET_SUPPORT_IPV6 == 1
        if( address.addr4.sin_family == AF_INET )
            return address.addr4.sin_addr.s_addr < right.address.addr4.sin_addr.s_addr;
        return memcmp( address.addr6.sin6_addr.s6_addr, right.address.addr6.sin6_addr.s6_addr, sizeof( address.addr6.sin6_addr.s6_addr ) ) > 0;
#else
        return address.addr4.sin_addr.s_addr < right.address.addr4.sin_addr.s_addr;
#endif
    }
    return address.addr4.sin_port < right.address.addr4.sin_port;
}
int SystemAddress::size( void )
{
#if RAKNET_SUPPORT_IPV6 == 1
    return sizeof( sockaddr_in6 ) + sizeof( char );
#else
    return sizeof( uint32_t ) + sizeof( unsigned short ) + sizeof( char );
#endif
}
unsigned long SystemAddress::ToInteger( const SystemAddress& sa )
{
    unsigned int lastHash = SuperFastHashIncremental( (const char*)&sa.address.addr4.sin_port, sizeof( sa.address.addr4.sin_port ), sizeof( sa.address.addr4.sin_port ) );
#if RAKNET_SUPPORT_IPV6 == 1
    if( sa.address.addr4.sin_family == AF_INET )
        return SuperFastHashIncremental( (const char*)&sa.address.addr4.sin_addr.s_addr, sizeof( sa.address.addr4.sin_addr.s_addr ), lastHash );
    else
        return SuperFastHashIncremental( (const char*)&sa.address.addr6.sin6_addr.s6_addr, sizeof( sa.address.addr6.sin6_addr.s6_addr ), lastHash );
#else
    return SuperFastHashIncremental( (const char*)&sa.address.addr4.sin_addr.s_addr, sizeof( sa.address.addr4.sin_addr.s_addr ), lastHash );
#endif
}
unsigned char SystemAddress::GetIPVersion( void ) const
{
    if( address.addr4.sin_family == AF_INET )
        return 4;
    return 6;
}
unsigned int SystemAddress::GetIPPROTO( void ) const
{
#if RAKNET_SUPPORT_IPV6 == 1
    if( address.addr4.sin_family == AF_INET )
        return IPPROTO_IP;
    return IPPROTO_IPV6;
#else
    return IPPROTO_IP;
#endif
}
void SystemAddress::SetToLoopback( void )
{
    SetToLoopback( GetIPVersion() );
}
void SystemAddress::SetToLoopback( unsigned char ipVersion )
{
    if( ipVersion == 4 )
    {
        FromString( IPV4_LOOPBACK, 0, ipVersion );
    }
    else
    {
        FromString( IPV6_LOOPBACK, 0, ipVersion );
    }
}
bool SystemAddress::IsLoopback( void ) const
{
    if( GetIPVersion() == 4 )
    {
        // unsigned long l = htonl(address.addr4.sin_addr.s_addr);
        if( htonl( address.addr4.sin_addr.s_addr ) == 2130706433 )
            return true;
        if( address.addr4.sin_addr.s_addr == 0 )
            return true;
    }
#if RAKNET_SUPPORT_IPV6 == 1
    else
    {
        const static char localhost[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
        if( memcmp( &address.addr6.sin6_addr, localhost, 16 ) == 0 )
            return true;
    }
#endif
    return false;
}
void SystemAddress::ToString_Old( bool writePort, char* dest, char portDelineator ) const
{
    if( *this == UNASSIGNED_SYSTEM_ADDRESS )
    {
        strcpy( dest, "UNASSIGNED_SYSTEM_ADDRESS" );
        return;
    }

    char portStr[2];
    portStr[0] = portDelineator;
    portStr[1] = 0;


    in_addr in;
    in.s_addr = address.addr4.sin_addr.s_addr;
    const char* ntoaStr = inet_ntoa( in );
    strcpy( dest, ntoaStr );
    if( writePort )
    {
        strcat( dest, portStr );
        char* portStart = dest + strlen( dest );
        auto res = std::to_chars( portStart, portStart + 5, GetPort() );
        RakAssert( res.ec == std::errc() );
        *res.ptr = '\0';
    }
}
const char* SystemAddress::ToString( bool writePort, char portDelineator ) const
{
    static unsigned char strIndex = 0;
#if RAKNET_SUPPORT_IPV6 == 1
    static char str[8][INET6_ADDRSTRLEN + 5 + 1];
#else
    static char str[8][22 + 5 + 1];
#endif

    unsigned char lastStrIndex = strIndex;
    strIndex++;
    ToString( writePort, str[lastStrIndex & 7], portDelineator );
    return (char*)str[lastStrIndex & 7];
}
#if RAKNET_SUPPORT_IPV6 == 1
void SystemAddress::ToString_New( bool writePort, char* dest, char portDelineator ) const
{
    int ret;
    (void)ret;

    if( *this == UNASSIGNED_SYSTEM_ADDRESS )
    {
        strcpy( dest, "UNASSIGNED_SYSTEM_ADDRESS" );
        return;
    }


    if( address.addr4.sin_family == AF_INET )
    {
        ret = getnameinfo( (struct sockaddr*)&address.addr4, sizeof( struct sockaddr_in ), dest, 22, NULL, 0, NI_NUMERICHOST );
    }
    else
    {
        ret = getnameinfo( (struct sockaddr*)&address.addr6, sizeof( struct sockaddr_in6 ), dest, INET6_ADDRSTRLEN, NULL, 0, NI_NUMERICHOST );
    }
    if( ret != 0 )
    {
        dest[0] = 0;
    }

    if( writePort )
    {
        unsigned char ch[2];
        ch[0] = portDelineator;
        ch[1] = 0;
        strcat( dest, (const char*)ch );
        char* portStart = dest + strlen( dest );
        auto res = std::to_chars( portStart, portStart + 5, ntohs( address.addr4.sin_port ) );
        RakAssert( res.ec == std::errc() );
        *res.ptr = '\0';
    }
}
#endif // #if RAKNET_SUPPORT_IPV6!=1
void SystemAddress::ToString( bool writePort, char* dest, char portDelineator ) const
{

#if RAKNET_SUPPORT_IPV6 != 1
    ToString_Old( writePort, dest, portDelineator );
#else
    ToString_New( writePort, dest, portDelineator );
#endif // #if RAKNET_SUPPORT_IPV6!=1
}
SystemAddress::SystemAddress()
{
    address.addr4.sin_family = AF_INET;
    // used for operator ==
    memset( &address, 0, sizeof( address ) );
    address.addr4.sin_family = AF_INET;
    systemIndex = (SystemIndex)-1;
    debugPort = 0;
}
SystemAddress::SystemAddress( const char* str )
{
    address.addr4.sin_family = AF_INET;
    SetPortHostOrder( 0 );
    FromString( str );
    systemIndex = (SystemIndex)-1;
}
SystemAddress::SystemAddress( const char* str, unsigned short port )
{
    address.addr4.sin_family = AF_INET;
    FromStringExplicitPort( str, port );
    systemIndex = (SystemIndex)-1;
}

void SystemAddress::FixForIPVersion( const SystemAddress& boundAddressToSocket )
{
    char str[128];
    ToString( false, str );
    // TODO - what about 255.255.255.255?
    if( strcmp( str, IPV6_LOOPBACK ) == 0 )
    {
        if( boundAddressToSocket.GetIPVersion() == 4 )
        {
            FromString( IPV4_LOOPBACK, 0, 4 );
        }
    }
    else if( strcmp( str, IPV4_LOOPBACK ) == 0 )
    {
#if RAKNET_SUPPORT_IPV6 == 1
        if( boundAddressToSocket.GetIPVersion() == 6 )
        {
            FromString( IPV6_LOOPBACK, 0, 6 );
        }
#endif

        //      if (boundAddressToSocket.GetIPVersion()==4)
        //      {
        //          // Some kind of bug with sendto: returns "The requested address is not valid in its context." if loopback doesn't have the same IP address
        //          address.addr4.sin_addr.s_addr=boundAddressToSocket.address.addr4.sin_addr.s_addr;
        //      }
    }
}
bool SystemAddress::IsLANAddress( void )
{
//  return address.addr4.sin_addr.S_un.S_un_b.s_b1==10 || address.addr4.sin_addr.S_un.s_b1==192;
#if defined( __WIN32__ )
    return address.addr4.sin_addr.S_un.S_un_b.s_b1 == 10 || address.addr4.sin_addr.S_un.S_un_b.s_b1 == 192;
#else
    return ( address.addr4.sin_addr.s_addr >> 24 ) == 10 || ( address.addr4.sin_addr.s_addr >> 24 ) == 192;
#endif
}
bool SystemAddress::SetBinaryAddress( const char* str, char portDelineator )
{
    const size_t hostLength = HostLength( str, portDelineator );
    if( hostLength > MaxHostNameLength )
        return false;

    std::optional<unsigned short> port;
    if( ReadPort( str[hostLength] ? str + hostLength + 1 : "", port ) == false )
        return false;

    std::optional<in_addr> ip;
    switch( ClassifyHost( str, hostLength ) )
    {
    case HostKind::Empty:
        break;
    case HostKind::IPv4: {
        char text[MaxNumericHostLength + 1];
        memcpy( text, str, hostLength );
        text[hostLength] = 0;
        ip.emplace();
        ip->s_addr = inet_addr__( text );
        break;
    }
    case HostKind::HostName: {
        if( EqualsIgnoringAsciiCase( std::string_view( str, hostLength ), "localhost" ) )
        {
            ip.emplace();
            ip->s_addr = inet_addr__( "127.0.0.1" );
            break;
        }

        char name[MaxHostNameLength + 1];
        memcpy( name, str, hostLength );
        name[hostLength] = 0;
        ip = ResolveIPv4( name );
        if( !ip )
            return false;
        break;
    }
    case HostKind::IPv6:
    case HostKind::Malformed:
        return false;
    }

    if( ip )
    {
        const unsigned short networkOrderPort = address.addr4.sin_port;
        memset( &address, 0, sizeof( address ) );
        address.addr4.sin_family = AF_INET;
        address.addr4.sin_port = networkOrderPort;
        address.addr4.sin_addr = *ip;
    }
    if( port )
        SetPortHostOrder( *port );
    return true;
}

bool SystemAddress::FromString( const char* str, char portDelineator, int ipVersion )
{
#if RAKNET_SUPPORT_IPV6 != 1
    (void)ipVersion;
    return SetBinaryAddress( str, portDelineator );
#else
    if( str == 0 )
    {
        memset( &address, 0, sizeof( address ) );
        address.addr4.sin_family = AF_INET;
        return true;
    }
    // Holds a numeric host or a host name.
    static_assert( MaxHostNameLength >= MaxNumericHostLength );
    char ipPart[MaxHostNameLength + 1];
    size_t hostLength = 0;
    bool numericHost = true;

    // TODO - what about 255.255.255.255?
    if( ipVersion == 4 && strcmp( str, IPV6_LOOPBACK ) == 0 )
    {
        strcpy( ipPart, IPV4_LOOPBACK );
    }
    else if( ipVersion == 6 && strcmp( str, IPV4_LOOPBACK ) == 0 )
    {
        address.addr4.sin_family = AF_INET6;
        strcpy( ipPart, IPV6_LOOPBACK );
    }
    else
    {
        hostLength = HostLength( str, portDelineator );
        if( hostLength > MaxHostNameLength )
            return false;
        switch( ClassifyHost( str, hostLength ) )
        {
        case HostKind::Malformed:
            return false;
        case HostKind::Empty:
        case HostKind::HostName:
            numericHost = false;
            break;
        case HostKind::IPv4:
        case HostKind::IPv6:
            break;
        }
        memcpy( ipPart, str, hostLength );
        ipPart[hostLength] = 0;
    }

    std::optional<unsigned short> port;
    if( ReadPort( ( str[hostLength] == portDelineator && portDelineator != 0 ) ? str + hostLength + 1 : "", port ) == false )
        return false;


    // needed for getaddrinfo
    WSAStartupSingleton::AddRef();

    // This could be a domain, or a printable address such as "192.0.2.1" or "2001:db8:63b3:1::3490"
    // I want to convert it to its binary representation
    addrinfo hints;
    memset( &hints, 0, sizeof hints );
    hints.ai_socktype = SOCK_DGRAM;
    if( numericHost )
        hints.ai_flags = AI_NUMERICHOST;
    if( ipVersion == 6 )
        hints.ai_family = AF_INET6;
    else if( ipVersion == 4 )
        hints.ai_family = AF_INET;
    else
        hints.ai_family = AF_UNSPEC;
    AddrInfoPtr servinfo = ResolveAddrInfo( ipPart, "", hints );
    if( !servinfo )
    {
        if( ipVersion == 6 )
        {
            ipVersion = 4;
            hints.ai_family = AF_UNSPEC;
            servinfo = ResolveAddrInfo( ipPart, "", hints );
            if( !servinfo )
            {
                WSAStartupSingleton::Deref();
                return false;
            }
        }
        else
        {
            WSAStartupSingleton::Deref();
            return false;
        }
    }
    RakAssert( servinfo );

    unsigned short oldPort = address.addr4.sin_port;
#if RAKNET_SUPPORT_IPV6 == 1
    if( servinfo->ai_family == AF_INET )
    {
        //      if (ipVersion==6)
        //      {
        //          address.addr4.sin_family=AF_INET6;
        //          memset(&address.addr6,0,sizeof(address.addr6));
        //          memcpy(address.addr6.sin6_addr.s6_addr+12,&((struct sockaddr_in *)servinfo->ai_addr)->sin_addr.s_addr,sizeof(unsigned long));
        //      }
        //      else
        //      {
        address.addr4.sin_family = AF_INET;
        memcpy( &address.addr4, (struct sockaddr_in*)servinfo->ai_addr, sizeof( struct sockaddr_in ) );
        //      }
    }
    else
    {
        address.addr4.sin_family = AF_INET6;
        memcpy( &address.addr6, (struct sockaddr_in6*)servinfo->ai_addr, sizeof( struct sockaddr_in6 ) );
    }
#else
    address.addr4.sin_family = AF_INET4;
    memcpy( &address.addr4, (struct sockaddr_in*)servinfo->ai_addr, sizeof( struct sockaddr_in ) );
#endif

    // Freed before the Deref, which can be the WSACleanup.
    servinfo.reset();

    // needed for getaddrinfo
    WSAStartupSingleton::Deref();

    // PORT
    if( port )
        SetPortHostOrder( *port );
    else
    {
        address.addr4.sin_port = oldPort;
    }
#endif // #if RAKNET_SUPPORT_IPV6!=1

    return true;
}
bool SystemAddress::FromStringExplicitPort( const char* str, unsigned short port, int ipVersion )
{
    bool b = FromString( str, (char)0, ipVersion );
    if( b == false )
    {
        *this = UNASSIGNED_SYSTEM_ADDRESS;
        return false;
    }
    address.addr4.sin_port = htons( port );
    debugPort = ntohs( address.addr4.sin_port );
    return true;
}
void SystemAddress::CopyPort( const SystemAddress& right )
{
    address.addr4.sin_port = right.address.addr4.sin_port;
    debugPort = right.debugPort;
}
RakNetGUID::RakNetGUID()
{
    systemIndex = (SystemIndex)-1;
    *this = UNASSIGNED_RAKNET_GUID;
}
bool RakNetGUID::operator==( const RakNetGUID& right ) const
{
    return g == right.g;
}
bool RakNetGUID::operator!=( const RakNetGUID& right ) const
{
    return g != right.g;
}
bool RakNetGUID::operator>( const RakNetGUID& right ) const
{
    return g > right.g;
}
bool RakNetGUID::operator<( const RakNetGUID& right ) const
{
    return g < right.g;
}
const char* RakNetGUID::ToString( void ) const
{
    static unsigned char strIndex = 0;
    static char str[8][64];

    unsigned char lastStrIndex = strIndex;
    strIndex++;
    ToString( str[lastStrIndex & 7] );
    return (char*)str[lastStrIndex & 7];
}
void RakNetGUID::ToString( char* dest ) const
{
    if( *this == UNASSIGNED_RAKNET_GUID )
        strcpy( dest, "UNASSIGNED_RAKNET_GUID" );
    else
    {
        // At most 20 digits.
        auto res = std::to_chars( dest, dest + 20, g );
        RakAssert( res.ec == std::errc() );
        *res.ptr = '\0';
    }
}
bool RakNetGUID::FromString( const char* source )
{
    if( source == 0 )
        return false;

    if( strcmp( source, "UNASSIGNED_RAKNET_GUID" ) == 0 )
    {
        g = UNASSIGNED_RAKNET_GUID.g;
        return true;
    }

    return ParseWholeNumber( source, g );
}
unsigned long RakNetGUID::ToUint32( const RakNetGUID& g )
{
    return ( (unsigned long)( g.g >> 32 ) ) ^ ( (unsigned long)( g.g & 0xFFFFFFFF ) );
}

} // namespace RakNet
