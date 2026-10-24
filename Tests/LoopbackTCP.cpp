#include "LoopbackTCP.h"

#include "ConnectionWaits.h"
#include "GetTime.h"
#include "SocketDefines.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>

namespace {

// How long ReceiveExactly's select waits before it checks its deadline again.
constexpr int kReceivePollMs = 10;

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

std::string LoopbackTCP::Client::ReceiveExactly( size_t length )
{
    std::string received;
    const RakNet::TimeMS deadline = RakNet::GetTimeMS() + kWaitBudget;
    while( received.size() < length )
    {
        if( ConnectionWaits::Expired( deadline ) )
            FAIL( "Timed out with " << received.size() << " of " << length << " bytes received" );

        // A short poll, so the deadline is checked between waits.
        timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = kReceivePollMs * 1000;
        fd_set readable;
        FD_ZERO( &readable );
        FD_SET( descriptor, &readable );
        if( select__( (int)descriptor + 1, &readable, 0, 0, &timeout ) <= 0 )
            continue;

        char chunk[4096];
        size_t wanted = length - received.size();
        if( wanted > sizeof( chunk ) )
            wanted = sizeof( chunk );
        const int got = recv__( descriptor, chunk, (int)wanted, 0 );
        if( got <= 0 )
            FAIL( "The connection closed with " << received.size() << " of " << length << " bytes received" );
        received.append( chunk, (size_t)got );
    }
    return received;
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
