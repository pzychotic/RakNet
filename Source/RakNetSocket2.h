/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#pragma once

#include "RakNetTypes.h"
#include "MTUSize.h"
#include "DS_Queue.h"
#include "Export.h"

#include <atomic>
#include <mutex>

namespace RakNet {

class RakNetSocket2;
struct RNS2_BerkleyBindParameters;
struct RNS2_SendParameters;
typedef int RNS2Socket;

enum RNS2BindResult
{
    BR_SUCCESS,
    BR_REQUIRES_RAKNET_SUPPORT_IPV6_DEFINE,
    BR_FAILED_TO_BIND_SOCKET,
    BR_FAILED_SEND_TEST,
};

typedef int RNS2SendResult;

struct RNS2_SendParameters
{
    RNS2_SendParameters() { ttl = 0; }
    char* data;
    int length;
    SystemAddress systemAddress;
    int ttl;
};

struct RNS2RecvStruct
{
    char data[MAXIMUM_MTU_SIZE];

    int bytesRead;
    SystemAddress systemAddress;
    RakNet::TimeUS timeRead;
    RakNetSocket2* socket;
};

class RakNetSocket2Allocator
{
public:
    static RakNetSocket2* AllocRNS2( void );
    static void DeallocRNS2( RakNetSocket2* s );
};

class RAK_DLL_EXPORT RNS2EventHandler
{
public:
    RNS2EventHandler() {}
    virtual ~RNS2EventHandler() {}

    virtual void OnRNS2Recv( RNS2RecvStruct* recvStruct ) = 0;
    virtual void DeallocRNS2RecvStruct( RNS2RecvStruct* s, const char* file, unsigned int line ) = 0;
    virtual RNS2RecvStruct* AllocRNS2RecvStruct( const char* file, unsigned int line ) = 0;
};

class RakNetSocket2
{
public:
    RakNetSocket2();
    virtual ~RakNetSocket2();

    // In order for the handler to trigger, some platforms must call PollRecvFrom, some platforms this create an internal thread.
    void SetRecvEventHandler( RNS2EventHandler* _eventHandler );
    virtual RNS2SendResult Send( RNS2_SendParameters* sendParameters, const char* file, unsigned int line ) = 0;
    bool IsBerkleySocket( void ) const;
    SystemAddress GetBoundAddress( void ) const;
    unsigned int GetUserConnectionSocketIndex( void ) const;
    void SetUserConnectionSocketIndex( unsigned int i );
    RNS2EventHandler* GetEventHandler( void ) const;

    // ----------- STATICS ------------
    static void GetMyIP( SystemAddress addresses[MAXIMUM_NUMBER_OF_INTERNAL_IDS] );
    static void DomainNameToIP( const char* domainName, char ip[65] );

protected:
    RNS2EventHandler* eventHandler;
    SystemAddress boundAddress;
    unsigned int userConnectionSocketIndex;
};


class RAK_DLL_EXPORT SocketLayerOverride
{
public:
    SocketLayerOverride() {}
    virtual ~SocketLayerOverride() {}

    /// Called when SendTo would otherwise occur.
    virtual int RakNetSendTo( const char* data, int length, const SystemAddress& systemAddress ) = 0;

    /// Called when RecvFrom would otherwise occur. Return number of bytes read. Write data into dataOut
    // Return -1 to use RakNet's normal recvfrom, 0 to abort RakNet's normal recvfrom, and positive to return data
    virtual int RakNetRecvFrom( char dataOut[MAXIMUM_MTU_SIZE], SystemAddress* senderOut, bool calledFromMainThread ) = 0;

    // RakNet needs to know whether an address is a dummy override address, so it won't be added as an external addresses
    virtual bool IsOverrideAddress( const SystemAddress& systemAddress ) const = 0;
};

struct RNS2_BerkleyBindParameters
{
    // Input parameters
    unsigned short port;
    char* hostAddress;
    unsigned short addressFamily; // AF_INET or AF_INET6
    int type;                     // SOCK_DGRAM
    int protocol;                 // 0
    bool nonBlockingSocket;
    int setBroadcast;
    int setIPHdrIncl;
    int doNotFragment;
    int pollingThreadPriority;
    RNS2EventHandler* eventHandler;
};

// Every platform that uses Berkley sockets, can compile some common functions
class RNS2_Berkley : public RakNetSocket2
{
public:
    RNS2_Berkley();
    virtual ~RNS2_Berkley();

    int CreateRecvPollingThread( int threadPriority );
    void SignalStopRecvPollingThread( void );
    void BlockOnStopRecvPollingThread( void );
    const RNS2_BerkleyBindParameters* GetBindings( void ) const;
    RNS2Socket GetSocket( void ) const;
    void SetDoNotFragment( int opt );

    RNS2BindResult Bind( RNS2_BerkleyBindParameters* bindParameters, const char* file, unsigned int line );
    RNS2SendResult Send( RNS2_SendParameters* sendParameters, const char* file, unsigned int line );

    void SetSocketLayerOverride( SocketLayerOverride* _slo );
    SocketLayerOverride* GetSocketLayerOverride( void );

    // For addressFamily, use AF_INET
    // For type, use SOCK_DGRAM
    static bool IsPortInUse( unsigned short port, const char* hostAddress, unsigned short addressFamily, int type );

protected:
    // Used by other classes
    RNS2BindResult BindShared( RNS2_BerkleyBindParameters* bindParameters, const char* file, unsigned int line );
    RNS2BindResult BindSharedIPV4( RNS2_BerkleyBindParameters* bindParameters, const char* file, unsigned int line );
    RNS2BindResult BindSharedIPV4And6( RNS2_BerkleyBindParameters* bindParameters, const char* file, unsigned int line );

    static RNS2SendResult Send_NoVDP( RNS2Socket rns2Socket, RNS2_SendParameters* sendParameters, const char* file, unsigned int line );

    static void GetSystemAddressIPV4( RNS2Socket rns2Socket, SystemAddress* systemAddressOut );
    static void GetSystemAddressIPV4And6( RNS2Socket rns2Socket, SystemAddress* systemAddressOut );

    // Internal
    void SetNonBlockingSocket( unsigned long nonblocking );
    void SetSocketOptions( void );
    void SetBroadcastSocket( int broadcast );
    void SetIPHdrIncl( int ipHdrIncl );
    void RecvFromBlocking( RNS2RecvStruct* recvFromStruct );
    void RecvFromBlockingIPV4( RNS2RecvStruct* recvFromStruct );
    void RecvFromBlockingIPV4And6( RNS2RecvStruct* recvFromStruct );

    RNS2Socket rns2Socket;
    RNS2_BerkleyBindParameters binding;

    unsigned RecvFromLoopInt( void );
    std::atomic<uint32_t> isRecvFromLoopThreadActive;
    volatile bool endThreads;
    // Constructor not called!

    SocketLayerOverride* slo;
    static void RecvFromLoop( void* arg );
};

} // namespace RakNet
