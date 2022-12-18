
#pragma once

// All this crap just to include type SOCKET

#if defined( _WIN32 )

// IP_DONTFRAGMENT is different between winsock 1 and winsock 2.  Therefore, Winsock2.h must be linked againt Ws2_32.lib
// winsock.h must be linked against WSock32.lib.  If these two are mixed up the flag won't work correctly
// WinRT: http://msdn.microsoft.com/en-us/library/windows/apps/windows.networking.sockets
// Sample code: http://stackoverflow.com/questions/10290945/correct-use-of-udp-datagramsocket
#include <winsock2.h>
typedef SOCKET __UDPSOCKET__;
typedef SOCKET __TCPSOCKET__;
typedef int socklen_t;

#else

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

//#include "RakMemoryOverride.h"
/// Unix/Linux uses ints for sockets
typedef int __UDPSOCKET__;
typedef int __TCPSOCKET__;

#endif
