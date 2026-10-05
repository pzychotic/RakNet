#include "RakNetDefines.h"
#include "RakNetTypes.h"
#include "SocketDefines.h"
#include "SocketIncludes.h"
#include "WSAStartupSingleton.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>

/*
WSAStartupSingleton counts the users of Winsock: the first AddRef calls WSAStartup and
the Deref that drops the last reference calls WSACleanup. Every RakPeer holds a
reference, and so does SystemAddress::FromString around getaddrinfo in IPv6 builds, so
references are taken and dropped on the user thread and the network threads at once.
A count that drifts below the number of holders runs WSACleanup under open sockets.

On platforms other than Windows the count stays 0 and these cases hold trivially.
*/

using namespace RakNet;

namespace {

struct WinsockRefCount : WSAStartupSingleton
{
    static int Get() { return refCount; }
};

} // namespace

TEST_CASE( "WSAStartupSingleton keeps its count when two threads take and drop references at once", "[socket]" )
{
    constexpr int kIterations = 200000;

    // Held throughout, so a correct count never reaches 0 and never cleans up.
    WSAStartupSingleton::AddRef();
    const int before = WinsockRefCount::Get();

    std::atomic<int> ready{ 0 };
    auto churn = [&] {
        ready.fetch_add( 1 );
        while( ready.load() < 2 )
            std::this_thread::yield();
        for( int i = 0; i < kIterations; ++i )
        {
            WSAStartupSingleton::AddRef();
            WSAStartupSingleton::Deref();
        }
    };
    std::thread first( churn );
    std::thread second( churn );
    first.join();
    second.join();

    CHECK( WinsockRefCount::Get() == before );

    // Winsock is still started: a socket can be created.
    const __UDPSOCKET__ probe = socket__( AF_INET, SOCK_DGRAM, 0 );
    CHECK( probe != INVALID_SOCKET );
    if( probe != INVALID_SOCKET )
        closesocket__( probe );

    WSAStartupSingleton::Deref();
}

#if RAKNET_SUPPORT_IPV6 == 1
TEST_CASE( "SystemAddress::FromString gives back its Winsock reference when the host does not resolve", "[socket]" )
{
    WSAStartupSingleton::AddRef();
    const int before = WinsockRefCount::Get();

    SystemAddress address;
    CHECK_FALSE( address.FromString( "nonexistent.invalid" ) );
    CHECK( WinsockRefCount::Get() == before );

    WSAStartupSingleton::Deref();
}
#endif
