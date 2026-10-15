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
/// \brief SocketLayer class implementation
///


#include "SocketLayer.h"
#include "RakAssert.h"
#include "RakNetSocket2.h"
#include "RakNetTypes.h"
#include "RakPeer.h"
#include "SocketDefines.h"
#if( defined( __GNUC__ ) || defined( __GCCXML__ ) ) && !defined( __WIN32__ )
#include <netdb.h>
#endif

#ifdef _WIN32
#else
#include <string.h> // memcpy
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <errno.h> // error numbers
#include <stdio.h> // RAKNET_DEBUG_PRINTF
#include <ifaddrs.h>
#include <netinet/in.h>
#include <net/if.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

#endif

#if defined( _WIN32 )
#include "WindowsIncludes.h"

#else
#include <unistd.h>
#endif

#include <stdio.h>

namespace RakNet {

void SocketLayer::SetSocketOptions( __UDPSOCKET__ listenSocket, bool blockingSocket, bool setBroadcast )
{
    // This doubles the max throughput rate
    int sock_opt = 1024 * 256;
    setsockopt__( listenSocket, SOL_SOCKET, SO_RCVBUF, (char*)&sock_opt, sizeof( sock_opt ) );

    // Immediate hard close. Don't linger the socket, or recreating the socket quickly on Vista fails.

    sock_opt = 0;
    setsockopt__( listenSocket, SOL_SOCKET, SO_LINGER, (char*)&sock_opt, sizeof( sock_opt ) );

    // This doesn't make much difference: 10% maybe
    sock_opt = 1024 * 16;
    setsockopt__( listenSocket, SOL_SOCKET, SO_SNDBUF, (char*)&sock_opt, sizeof( sock_opt ) );

    if( blockingSocket == false )
    {
#ifdef _WIN32
        unsigned long nonblocking = 1;
        ioctlsocket__( listenSocket, FIONBIO, &nonblocking );
#else
        fcntl( listenSocket, F_SETFL, O_NONBLOCK );
#endif
    }
    if( setBroadcast )
    {
        // Note: Fails with VDP
        // Set broadcast capable
        sock_opt = 1;
        if( setsockopt__( listenSocket, SOL_SOCKET, SO_BROADCAST, (char*)&sock_opt, sizeof( sock_opt ) ) == -1 )
        {
#if defined( _WIN32 ) && defined( _DEBUG )
            DWORD dwIOError = GetLastError();
            // On Vista, can get WSAEACCESS (10013)
            // See http://support.microsoft.com/kb/819124
            // http://blogs.msdn.com/wndp/archive/2007/03/19/winsock-so-exclusiveaddruse-on-vista.aspx
            // http://msdn.microsoft.com/en-us/library/ms740621(VS.85).aspx
            LPSTR messageBuffer;
            FormatMessage( FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                           NULL, dwIOError, MAKELANGID( LANG_NEUTRAL, SUBLANG_DEFAULT ), // Default language
                           (LPTSTR)&messageBuffer, 0, NULL );
            // something has gone wrong here...
            RAKNET_DEBUG_PRINTF( "setsockopt__(SO_BROADCAST) failed:Error code - %d\n%s", dwIOError, messageBuffer );
            //Free the buffer.
            LocalFree( messageBuffer );
#endif
        }
    }
}

unsigned short SocketLayer::GetLocalPort( __UDPSOCKET__ s )
{
    SystemAddress sa;
    GetSystemAddress( s, &sa );
    return sa.GetPort();
}

#if RAKNET_SUPPORT_IPV6 != 1
static void GetSystemAddress_Old( __UDPSOCKET__ s, SystemAddress* systemAddressOut )
{
    sockaddr_in sa;
    memset( &sa, 0, sizeof( sockaddr_in ) );
    socklen_t len = sizeof( sa );
    if( getsockname__( s, (sockaddr*)&sa, &len ) != 0 )
    {
#if defined( _WIN32 ) && defined( _DEBUG )
        DWORD dwIOError = GetLastError();
        LPSTR messageBuffer;
        FormatMessage( FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, dwIOError, MAKELANGID( LANG_NEUTRAL, SUBLANG_DEFAULT ), // Default language
                       (LPTSTR)&messageBuffer, 0, NULL );
        // something has gone wrong here...
        RAKNET_DEBUG_PRINTF( "getsockname failed:Error code - %d\n%s", dwIOError, messageBuffer );

        //Free the buffer.
        LocalFree( messageBuffer );
#endif
        *systemAddressOut = UNASSIGNED_SYSTEM_ADDRESS;
        return;
    }

    systemAddressOut->SetPortNetworkOrder( sa.sin_port );
    systemAddressOut->address.addr4.sin_addr.s_addr = sa.sin_addr.s_addr;
}
#endif

void SocketLayer::GetSystemAddress( __UDPSOCKET__ s, SystemAddress* systemAddressOut )
{
#if RAKNET_SUPPORT_IPV6 != 1
    GetSystemAddress_Old( s, systemAddressOut );
#else
    socklen_t slen;
    sockaddr_storage ss;
    slen = sizeof( ss );

    if( getsockname__( s, (struct sockaddr*)&ss, &slen ) != 0 )
    {
#if defined( _WIN32 ) && defined( _DEBUG )
        DWORD dwIOError = GetLastError();
        LPSTR messageBuffer;
        FormatMessage( FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, dwIOError, MAKELANGID( LANG_NEUTRAL, SUBLANG_DEFAULT ), // Default language
                       (LPTSTR)&messageBuffer, 0, NULL );
        // something has gone wrong here...
        RAKNET_DEBUG_PRINTF( "getsockname failed:Error code - %d\n%s", dwIOError, messageBuffer );

        //Free the buffer.
        LocalFree( messageBuffer );
#endif
        systemAddressOut->FromString( 0 );
        return;
    }

    if( ss.ss_family == AF_INET )
    {
        memcpy( &systemAddressOut->address.addr4, (sockaddr_in*)&ss, sizeof( sockaddr_in ) );
        systemAddressOut->debugPort = ntohs( systemAddressOut->address.addr4.sin_port );

        if( IsWildcardAddress( *systemAddressOut ) )
            systemAddressOut->SetToLoopback( 4 );
        //  systemAddressOut->address.addr4.sin_port=ntohs(systemAddressOut->address.addr4.sin_port);
    }
    else
    {
        memcpy( &systemAddressOut->address.addr6, (sockaddr_in6*)&ss, sizeof( sockaddr_in6 ) );
        systemAddressOut->debugPort = ntohs( systemAddressOut->address.addr6.sin6_port );

        if( IsWildcardAddress( *systemAddressOut ) )
            systemAddressOut->SetToLoopback( 6 );

        //  systemAddressOut->address.addr6.sin6_port=ntohs(systemAddressOut->address.addr6.sin6_port);
    }
#endif // #if RAKNET_SUPPORT_IPV6!=1
}

} // namespace RakNet
