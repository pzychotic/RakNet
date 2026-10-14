#pragma once

#include "SocketDefines.h"
#include "SocketIncludes.h"

#if defined( _WIN32 )
#include "WindowsIncludes.h"
#else
#include <netdb.h>
#endif

#include <memory>

namespace RakNet {

/// Owns a list from getaddrinfo and frees it with freeaddrinfo.
using AddrInfoPtr = std::unique_ptr<addrinfo, decltype( &freeaddrinfo )>;

/// Resolves node and service with getaddrinfo. The handle is empty when the call fails.
inline AddrInfoPtr ResolveAddrInfo( const char* node, const char* service, const addrinfo& hints )
{
    addrinfo* list = nullptr;
    if( getaddrinfo( node, service, &hints, &list ) != 0 )
        list = nullptr;
    return AddrInfoPtr( list, &freeaddrinfo );
}

/// Walks list and returns the first socket that socket__ creates and bind__ accepts, or
/// INVALID_SOCKET if none does. Every socket it rejects is closed. prepare, if given, is
/// called on each socket before it is bound.
inline __UDPSOCKET__ BindFirstAddress( const addrinfo* list, void ( *prepare )( __UDPSOCKET__ ) = nullptr )
{
    for( const addrinfo* aip = list; aip != nullptr; aip = aip->ai_next )
    {
        __UDPSOCKET__ s = socket__( aip->ai_family, aip->ai_socktype, aip->ai_protocol );
        if( s == INVALID_SOCKET )
            continue;
        if( prepare != nullptr )
            prepare( s );
        if( bind__( s, aip->ai_addr, (int)aip->ai_addrlen ) >= 0 )
            return s;
        closesocket__( s );
    }
    return INVALID_SOCKET;
}

} // namespace RakNet
