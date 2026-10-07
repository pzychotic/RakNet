#include "LoopbackTCP.h"

#include "SocketDefines.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>

namespace {

sockaddr_in LoopbackAddress( unsigned short port )
{
    sockaddr_in address;
    memset( &address, 0, sizeof( address ) );
    address.sin_family = AF_INET;
    address.sin_port = htons( port );
    address.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
    return address;
}

} // namespace

bool LoopbackTCP::IsSocket( __TCPSOCKET__ descriptor )
{
    int type = 0;
    socklen_t length = sizeof( type );
    return getsockopt__( descriptor, SOL_SOCKET, SO_TYPE, (char*)&type, &length ) == 0;
}

LoopbackTCP::Client::Client( unsigned short listenPort, unsigned short clientPort, int bufferSize )
: descriptor( socket__( AF_INET, SOCK_STREAM, IPPROTO_TCP ) )
{
    REQUIRE( descriptor != INVALID_SOCKET );

    if( bufferSize != kDefaultBuffers )
    {
        setsockopt__( descriptor, SOL_SOCKET, SO_SNDBUF, (const char*)&bufferSize, sizeof( bufferSize ) );
        setsockopt__( descriptor, SOL_SOCKET, SO_RCVBUF, (const char*)&bufferSize, sizeof( bufferSize ) );
    }

    // A constructor that throws runs no destructor, so a failed step closes the
    // socket itself before it FAILs.
    if( clientPort != kAnyPort )
    {
        const int reuse = 1;
        if( setsockopt__( descriptor, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof( reuse ) ) != 0 )
        {
            Abort();
            FAIL( "SO_REUSEADDR failed on the client socket for port " << clientPort );
        }

        const sockaddr_in local = LoopbackAddress( clientPort );
        if( bind__( descriptor, (const sockaddr*)&local, sizeof( local ) ) != 0 )
        {
            Abort();
            FAIL( "The client socket could not bind 127.0.0.1:" << clientPort );
        }
    }

    const sockaddr_in remote = LoopbackAddress( listenPort );
    if( connect__( descriptor, (const sockaddr*)&remote, sizeof( remote ) ) != 0 )
    {
        Abort();
        FAIL( "The client socket could not connect to 127.0.0.1:" << listenPort );
    }
}

LoopbackTCP::Client::~Client()
{
    Abort();
}

LoopbackTCP::Client::Client( Client&& other ) noexcept
: descriptor( other.descriptor )
{
    other.descriptor = INVALID_SOCKET;
}

void LoopbackTCP::Client::SendAll( const char* data, size_t length )
{
    while( length != 0 )
    {
        const int sent = send__( descriptor, data, (int)length, 0 );
        REQUIRE( sent > 0 );
        data += sent;
        length -= (size_t)sent;
    }
}

void LoopbackTCP::Client::Abort()
{
    if( descriptor == INVALID_SOCKET )
        return;

    linger abortive;
    abortive.l_onoff = 1;
    abortive.l_linger = 0;
    setsockopt__( descriptor, SOL_SOCKET, SO_LINGER, (const char*)&abortive, sizeof( abortive ) );
    closesocket__( descriptor );
    descriptor = INVALID_SOCKET;
}
