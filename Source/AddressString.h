#pragma once

#include <charconv>
#include <cstddef>
#include <cstring>

#if defined( _WIN32 )
#include "WindowsIncludes.h"
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#endif

namespace RakNet {

/// What the host of an address string is. The host is everything before the port
/// delineator.
enum class HostKind
{
    /// Names no address.
    Empty,
    /// A dotted quad of four decimal octets 0-255, with no leading zeros.
    IPv4,
    /// An address inet_pton( AF_INET6 ) accepts, optionally followed by '%' and a zone.
    IPv6,
    /// Looks like a numeric host, because it contains ':' or its last label is all digits,
    /// but isn't one.
    Malformed,
    /// Any other non-empty host, for a resolver to look up.
    HostName,
};

/// The longest zone after an IPv6 address's '%': Linux's IF_NAMESIZE less its NUL.
constexpr size_t MaxZoneLength = 15;

/// The longest IPv6 address: INET6_ADDRSTRLEN less its NUL.
constexpr size_t MaxIPv6AddressLength = 45;

/// The longest numeric host: an IPv6 address, '%' and a zone.
constexpr size_t MaxNumericHostLength = MaxIPv6AddressLength + 1 + MaxZoneLength;

namespace AddressStringDetail {

inline bool IsDigit( char c )
{
    return c >= '0' && c <= '9';
}

inline bool IsDottedQuad( const char* host, size_t length )
{
    const char* p = host;
    const char* const end = host + length;
    for( int octet = 0; octet < 4; octet++ )
    {
        if( octet > 0 )
        {
            if( p == end || *p != '.' )
                return false;
            ++p;
        }
        if( p == end || IsDigit( *p ) == false )
            return false;
        unsigned int value = 0;
        auto res = std::from_chars( p, end, value );
        if( res.ec != std::errc() || value > 255 )
            return false;
        if( *p == '0' && res.ptr - p > 1 )
            return false;
        p = res.ptr;
    }
    return p == end;
}

inline bool IsZoneCharacter( char c )
{
    return IsDigit( c ) || ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || c == '.' || c == '_' || c == '-';
}

inline bool IsIPv6Host( const char* host, size_t length )
{
    const char* const percent = static_cast<const char*>( memchr( host, '%', length ) );
    const size_t addressLength = percent ? static_cast<size_t>( percent - host ) : length;
    char address[MaxIPv6AddressLength + 1];
    if( addressLength >= sizeof( address ) )
        return false;
    memcpy( address, host, addressLength );
    address[addressLength] = 0;
    unsigned char binary[16];
    if( inet_pton( AF_INET6, address, binary ) != 1 )
        return false;
    if( percent == nullptr )
        return true;
    const size_t zoneLength = length - addressLength - 1;
    if( zoneLength == 0 || zoneLength > MaxZoneLength )
        return false;
    for( size_t i = addressLength + 1; i < length; i++ )
    {
        if( IsZoneCharacter( host[i] ) == false )
            return false;
    }
    return true;
}

// Whether the host's last label, once one trailing dot is dropped, is all digits.
inline bool EndsInNumber( const char* host, size_t length )
{
    if( length > 0 && host[length - 1] == '.' )
        length--;
    size_t start = length;
    while( start > 0 && host[start - 1] != '.' )
        start--;
    if( start == length )
        return false;
    for( size_t i = start; i < length; i++ )
    {
        if( IsDigit( host[i] ) == false )
            return false;
    }
    return true;
}

} // namespace AddressStringDetail

/// Classifies the \a length characters at \a host. Allocates nothing and asks no resolver.
inline HostKind ClassifyHost( const char* host, size_t length )
{
    using namespace AddressStringDetail;
    if( length == 0 )
        return HostKind::Empty;
    if( IsDottedQuad( host, length ) )
        return HostKind::IPv4;
    if( IsIPv6Host( host, length ) )
        return HostKind::IPv6;
    if( memchr( host, ':', length ) != nullptr || EndsInNumber( host, length ) )
        return HostKind::Malformed;
    return HostKind::HostName;
}

} // namespace RakNet
