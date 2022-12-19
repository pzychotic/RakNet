/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "RakNetSocket2.h"
#include "RakMemoryOverride.h"
#include "RakAssert.h"
#include "RakSleep.h"
#include "RakThread.h"
#include "SocketDefines.h"
#include "GetTime.h"
#include <stdio.h>
#include <string.h> // memcpy

#ifdef _WIN32
#else
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <errno.h> // error numbers
#include <ifaddrs.h>
#include <netinet/in.h>
#include <net/if.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#endif

#define RAKNET_SOCKET_2_INLINE_FUNCTIONS
#include "RakNetSocket2_Berkley.cpp"
#undef RAKNET_SOCKET_2_INLINE_FUNCTIONS

#ifndef INVALID_SOCKET
#define INVALID_SOCKET -1
#endif

namespace RakNet {

RakNetSocket2* RakNetSocket2Allocator::AllocRNS2( void )
{
    return RakNet::OP_NEW<RNS2_Berkley>( _FILE_AND_LINE_ );
}
void RakNetSocket2Allocator::DeallocRNS2( RakNetSocket2* s )
{
    RakNet::OP_DELETE( s, _FILE_AND_LINE_ );
}

RakNetSocket2::RakNetSocket2() { eventHandler = 0; }
RakNetSocket2::~RakNetSocket2() {}
void RakNetSocket2::SetRecvEventHandler( RNS2EventHandler* _eventHandler ) { eventHandler = _eventHandler; }
bool RakNetSocket2::IsBerkleySocket( void ) const
{
    return true;
}
SystemAddress RakNetSocket2::GetBoundAddress( void ) const { return boundAddress; }

void RakNetSocket2::GetMyIP( SystemAddress addresses[MAXIMUM_NUMBER_OF_INTERNAL_IDS] )
{
    return GetMyIP_Windows_Linux( addresses );
}

unsigned int RakNetSocket2::GetUserConnectionSocketIndex( void ) const { return userConnectionSocketIndex; }
void RakNetSocket2::SetUserConnectionSocketIndex( unsigned int i ) { userConnectionSocketIndex = i; }
RNS2EventHandler* RakNetSocket2::GetEventHandler( void ) const { return eventHandler; }

void RakNetSocket2::DomainNameToIP( const char* domainName, char ip[65] )
{
    return DomainNameToIP_Berkley( domainName, ip );
}

bool RNS2_Berkley::IsPortInUse( unsigned short port, const char* hostAddress, unsigned short addressFamily, int type )
{
    RNS2_BerkleyBindParameters bbp;
    bbp.port = port;
    bbp.hostAddress = (char*)hostAddress;
    bbp.addressFamily = addressFamily;
    bbp.type = type;
    bbp.protocol = 0;
    bbp.nonBlockingSocket = false;
    bbp.setBroadcast = false;
    bbp.doNotFragment = false;
    bbp.protocol = 0;
    bbp.setIPHdrIncl = false;
    SystemAddress boundAddress;
    RNS2_Berkley* rns2 = (RNS2_Berkley*)RakNetSocket2Allocator::AllocRNS2();
    RNS2BindResult bindResult = rns2->Bind( &bbp, _FILE_AND_LINE_ );
    RakNetSocket2Allocator::DeallocRNS2( rns2 );
    return bindResult == BR_FAILED_TO_BIND_SOCKET;
}

RNS2BindResult RNS2_Berkley::BindShared( RNS2_BerkleyBindParameters* bindParameters, const char* file, unsigned int line )
{
    RNS2BindResult br;
#if RAKNET_SUPPORT_IPV6 == 1
    br = BindSharedIPV4And6( bindParameters, file, line );
#else
    br = BindSharedIPV4( bindParameters, file, line );
#endif

    if( br != BR_SUCCESS )
        return br;

    unsigned long zero = 0;
    RNS2_SendParameters bsp;
    bsp.data = (char*)&zero;
    bsp.length = 4;
    bsp.systemAddress = boundAddress;
    bsp.ttl = 0;
    RNS2SendResult sr = Send( &bsp, _FILE_AND_LINE_ );
    if( sr < 0 )
        return BR_FAILED_SEND_TEST;

    memcpy( &binding, bindParameters, sizeof( RNS2_BerkleyBindParameters ) );

    return br;
}

void RNS2_Berkley::RecvFromLoop( void* arg )
{
    RNS2_Berkley* b = (RNS2_Berkley*)arg;

    b->RecvFromLoopInt();
}
unsigned RNS2_Berkley::RecvFromLoopInt( void )
{
    isRecvFromLoopThreadActive++;

    while( endThreads == false )
    {
        RNS2RecvStruct* recvFromStruct;
        recvFromStruct = binding.eventHandler->AllocRNS2RecvStruct( _FILE_AND_LINE_ );
        if( recvFromStruct != NULL )
        {
            recvFromStruct->socket = this;
            RecvFromBlocking( recvFromStruct );

            if( recvFromStruct->bytesRead > 0 )
            {
                RakAssert( recvFromStruct->systemAddress.GetPort() );
                binding.eventHandler->OnRNS2Recv( recvFromStruct );
            }
            else
            {
                RakSleep( 0 );
                binding.eventHandler->DeallocRNS2RecvStruct( recvFromStruct, _FILE_AND_LINE_ );
            }
        }
    }
    isRecvFromLoopThreadActive--;

    return 0;
}
RNS2_Berkley::RNS2_Berkley()
{
    rns2Socket = (RNS2Socket)INVALID_SOCKET;
    slo = 0;
}
RNS2_Berkley::~RNS2_Berkley()
{
    if( rns2Socket != INVALID_SOCKET )
    {
        closesocket__( rns2Socket );
    }
}
int RNS2_Berkley::CreateRecvPollingThread( int threadPriority )
{
    endThreads = false;

    int errorCode = RakThread::Create( RecvFromLoop, this, threadPriority );

    return errorCode;
}
void RNS2_Berkley::SignalStopRecvPollingThread( void )
{
    endThreads = true;
}
void RNS2_Berkley::BlockOnStopRecvPollingThread( void )
{
    endThreads = true;

    // Get recvfrom to unblock
    RNS2_SendParameters bsp;
    unsigned long zero = 0;
    bsp.data = (char*)&zero;
    bsp.length = 4;
    bsp.systemAddress = boundAddress;
    bsp.ttl = 0;
    Send( &bsp, _FILE_AND_LINE_ );

    RakNet::TimeMS timeout = RakNet::GetTimeMS() + 1000;
    while( isRecvFromLoopThreadActive > 0 && RakNet::GetTimeMS() < timeout )
    {
        // Get recvfrom to unblock
        Send( &bsp, _FILE_AND_LINE_ );
        RakSleep( 30 );
    }
}
const RNS2_BerkleyBindParameters* RNS2_Berkley::GetBindings( void ) const { return &binding; }
RNS2Socket RNS2_Berkley::GetSocket( void ) const { return rns2Socket; }

void RNS2_Berkley::SetSocketLayerOverride( SocketLayerOverride* _slo ) { slo = _slo; }
SocketLayerOverride* RNS2_Berkley::GetSocketLayerOverride( void ) { return slo; }

// See RakNetSocket2_Berkley.cpp for BindSharedIPV4And6 and other implementations

RNS2BindResult RNS2_Berkley::Bind( RNS2_BerkleyBindParameters* bindParameters, const char* file, unsigned int line )
{
#if defined( _WIN32 )
    RNS2BindResult bindResult = BindShared( bindParameters, file, line );
    if( bindResult == BR_FAILED_TO_BIND_SOCKET )
    {
        // Sometimes windows will fail if the socket is recreated too quickly
        RakSleep( 100 );
        bindResult = BindShared( bindParameters, file, line );
    }
    return bindResult;
#else
    return BindShared( bindParameters, file, line );
#endif
}

RNS2SendResult RNS2_Berkley::Send( RNS2_SendParameters* sendParameters, const char* file, unsigned int line )
{
    if( slo )
    {
        RNS2SendResult len;
        len = slo->RakNetSendTo( sendParameters->data, sendParameters->length, sendParameters->systemAddress );
        if( len >= 0 )
            return len;
    }
    return Send_NoVDP( rns2Socket, sendParameters, file, line );
}

} // namespace RakNet
