#pragma once

#include "WSAStartupSingleton.h"

/*
 *  Holds a reference to Winsock for the lifetime of a Catch2 test case, and
 *  releases it on the way out - normally, or by exception. A no-op off Windows.
 *
 *  A RakPeer or TCPInterface holds this same reference for as long as it
 *  exists. A test needs its own when it reaches the socket API with neither
 *  alive, including sendto on a socket it never opened and the inet_* address
 *  conversions, which Winsock documents as requiring WSAStartup too.
 *
 *  Declared as a plain local, first line of the test body, for the same reasons
 *  PeerScope is:
 *
 *      TEST_CASE( "...", "[socket]" )
 *      {
 *          WinsockScope winsock;
 *          ...
 *      }
 *
 *  RawSystem holds one, so a test built on a RawSystem needs none of its own.
 */
class WinsockScope
{
public:
    WinsockScope() { RakNet::WSAStartupSingleton::AddRef(); }
    ~WinsockScope() { RakNet::WSAStartupSingleton::Deref(); }

    WinsockScope( const WinsockScope& ) = delete;
    WinsockScope& operator=( const WinsockScope& ) = delete;
};
