/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "NativeFeatureIncludes.h"
#if _RAKNET_SUPPORT_TCPInterface == 1

/// \file
/// \brief A simple TCP based server allowing sends and receives.  Can be connected to by a telnet client.
///

#include "TCPInterface.h"
#ifdef _WIN32
typedef int socklen_t;
#else
#include <sys/time.h>
#include <unistd.h>
#include <pthread.h>
#endif
#include <string.h>
#include "RakAssert.h"
#include <stdio.h>
#include "RakThread.h"
#include "StringCompressor.h"
#include "SocketLayer.h"
#include "SocketDefines.h"
#if( defined( __GNUC__ ) || defined( __GCCXML__ ) ) && !defined( __WIN32__ )
#include <netdb.h>
#endif

#ifdef _DO_PRINTF
#endif

#ifdef _WIN32
#include "WSAStartupSingleton.h"
#endif

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <thread>

namespace RakNet {

void UpdateTCPInterfaceLoop( void* arg );
void ConnectionAttemptLoop( void* arg );

namespace {

bool SetSocketBlocking( __TCPSOCKET__ socket, bool isBlocking )
{
#ifdef _WIN32
    u_long isNonBlocking = isBlocking ? 0 : 1;
    return ioctlsocket__( socket, FIONBIO, &isNonBlocking ) == 0;
#else
    const int flags = fcntl( socket, F_GETFL, 0 );
    if( flags == -1 )
        return false;
    return fcntl( socket, F_SETFL, isBlocking ? ( flags & ~O_NONBLOCK ) : ( flags | O_NONBLOCK ) ) == 0;
#endif
}

// Whether a non-blocking connect that did not connect at once is still going.
bool IsConnectInProgress( void )
{
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EINPROGRESS || errno == EINTR;
#endif
}

bool IsInterruptedCall( void )
{
#ifdef _WIN32
    return WSAGetLastError() == WSAEINTR;
#else
    return errno == EINTR;
#endif
}

} // namespace

STATIC_FACTORY_DEFINITIONS( TCPInterface, TCPInterface );

TCPInterface::TCPInterface()
{
    isStarted = 0;
    threadRunning = 0;
    isStopping = false;
    connectAttemptCount = 0;
    listenSocket = INVALID_SOCKET;
    remoteClients = 0;
    remoteClientsLength = 0;
    maxIncomingBytesPerClient = DEFAULT_MAXIMUM_INCOMING_BYTES_PER_CLIENT;
    maxOutgoingBytesPerClient = DEFAULT_MAXIMUM_OUTGOING_BYTES_PER_CLIENT;
    incomingBytesCapStallCount = 0;
    outgoingBytesCapCloseCount = 0;

    StringCompressor::AddReference();

#if OPEN_SSL_CLIENT_SUPPORT == 1
    ctx = 0;
    meth = 0;
#endif

#ifdef _WIN32
    WSAStartupSingleton::AddRef();
#endif
}
TCPInterface::~TCPInterface()
{
    Stop();
#ifdef _WIN32
    WSAStartupSingleton::Deref();
#endif

    RakNet::OP_DELETE_ARRAY( remoteClients, _FILE_AND_LINE_ );

    StringCompressor::RemoveReference();
}

bool TCPInterface::CreateListenSocket( unsigned short port, unsigned short maxIncomingConnections, unsigned short socketFamily, const char* bindAddress )
{
    (void)maxIncomingConnections;
    (void)socketFamily;
#if RAKNET_SUPPORT_IPV6 != 1
    listenSocket = socket__( AF_INET, SOCK_STREAM, 0 );
    if( listenSocket == INVALID_SOCKET )
        return false;

    struct sockaddr_in serverAddress;
    memset( &serverAddress, 0, sizeof( sockaddr_in ) );
    serverAddress.sin_family = AF_INET;
    if( bindAddress && bindAddress[0] )
    {
        serverAddress.sin_addr.s_addr = inet_addr__( bindAddress );
    }
    else
        serverAddress.sin_addr.s_addr = INADDR_ANY;

    serverAddress.sin_port = htons( port );

    SocketLayer::SetSocketOptions( listenSocket, false, false );

    if( bind__( listenSocket, (struct sockaddr*)&serverAddress, sizeof( serverAddress ) ) < 0 )
        return false;

    listen__( listenSocket, maxIncomingConnections );
#else
    struct addrinfo hints;
    memset( &hints, 0, sizeof( addrinfo ) ); // make sure the struct is empty
    hints.ai_family = socketFamily;          // don't care IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;         // TCP sockets
    hints.ai_flags = AI_PASSIVE;             // fill in my IP for me
    struct addrinfo *servinfo = 0, *aip;     // will point to the results
    char portStr[32];
    auto res = std::to_chars( portStr, portStr + 31, port );
    RakAssert( res.ec == std::errc() );
    *res.ptr = '\0';

    getaddrinfo( 0, portStr, &hints, &servinfo );
    for( aip = servinfo; aip != NULL; aip = aip->ai_next )
    {
        // Open socket. The address type depends on what
        // getaddrinfo() gave us.
        listenSocket = socket__( aip->ai_family, aip->ai_socktype, aip->ai_protocol );
        if( listenSocket != INVALID_SOCKET )
        {
            int ret = bind__( listenSocket, aip->ai_addr, (int)aip->ai_addrlen );
            if( ret >= 0 )
            {
                break;
            }
            else
            {
                closesocket__( listenSocket );
                listenSocket = INVALID_SOCKET;
            }
        }
    }

    if( listenSocket == INVALID_SOCKET )
        return false;

    SocketLayer::SetSocketOptions( listenSocket, false, false );

    listen__( listenSocket, maxIncomingConnections );
#endif // #if RAKNET_SUPPORT_IPV6!=1

    return true;
}

bool TCPInterface::Start( unsigned short port, unsigned short maxIncomingConnections, unsigned short maxConnections, int _threadPriority, unsigned short socketFamily, const char* bindAddress )
{
    if( isStarted > 0 )
        return false;

    threadPriority = _threadPriority;

    if( threadPriority == -99999 )
    {
#if defined( _WIN32 )
        threadPriority = 0;
#else
        threadPriority = 1000;
#endif
    }

    isStarted++;
    if( maxConnections == 0 )
        maxConnections = maxIncomingConnections;
    if( maxConnections == 0 )
        maxConnections = 1;
    remoteClientsLength = maxConnections;
    remoteClients = RakNet::OP_NEW_ARRAY<RemoteClient>( maxConnections, _FILE_AND_LINE_ );

    listenSocket = INVALID_SOCKET;
    if( maxIncomingConnections > 0 )
    {
        CreateListenSocket( port, maxIncomingConnections, socketFamily, bindAddress );
    }

    // Start the update thread
    int errorCode = RakThread::Create( UpdateTCPInterfaceLoop, this, threadPriority );

    if( errorCode != 0 )
        return false;

    while( threadRunning == 0 )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 0 ) );
    }

    for( PluginInterface2* pPlugin : messageHandlerList )
    {
        pPlugin->OnRakPeerStartup();
    }

    return true;
}
void TCPInterface::Stop( void )
{
    const bool wasStarted = isStarted > 0;

    // Set before the plugins hear of the shutdown, so a Connect from one is refused too.
    if( wasStarted )
    {
        std::lock_guard<std::mutex> guard( connectAttemptMutex );
        isStopping = true;
    }

    for( PluginInterface2* pPlugin : messageHandlerList )
    {
        pPlugin->OnRakPeerShutdown();
    }

    if( wasStarted == false )
        return;

#if OPEN_SSL_CLIENT_SUPPORT == 1
    for( int i = 0; i < remoteClientsLength; i++ )
    {
        std::lock_guard<std::mutex> guard( remoteClients[i].isActiveMutex );
        remoteClients[i].DisconnectSSL();
    }
#endif

    isStarted--;

    if( listenSocket != INVALID_SOCKET )
    {
#ifdef _WIN32
        shutdown__( listenSocket, SD_BOTH );

#else
        shutdown__( listenSocket, SHUT_RDWR );
#endif
    }

    // Every connect attempt ends before anything it touches is freed. One still connecting
    // sees isStopping and gives up, closing its own socket; one that won has put its socket
    // in its connection record, and the Free() loop below closes it.
    {
        std::unique_lock<std::mutex> lock( connectAttemptMutex );
        connectAttemptEnded.wait( lock, [this] { return connectAttemptCount == 0; } );
    }

    // Wait for the thread to stop
    while( threadRunning > 0 )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 15 ) );
    }

    // Closed only once the update thread is gone, so it never accepts on a descriptor
    // number that has been released for reuse.
    if( listenSocket != INVALID_SOCKET )
        closesocket__( listenSocket );
    listenSocket = INVALID_SOCKET;

    // Stuff from here on to the end of the function is not threadsafe
    for( int i = 0; i < remoteClientsLength; i++ )
    {
        std::lock_guard<std::mutex> guard( remoteClients[i].isActiveMutex );
        remoteClients[i].Free();
    }
    remoteClientsLength = 0;
    RakNet::OP_DELETE_ARRAY( remoteClients, _FILE_AND_LINE_ );
    remoteClients = 0;

    incomingMessages.Clear( _FILE_AND_LINE_ );
    newIncomingConnections.Clear( _FILE_AND_LINE_ );
    newRemoteClients.Clear( _FILE_AND_LINE_ );
    lostConnections.Clear( _FILE_AND_LINE_ );
    requestedCloseConnections.Clear( _FILE_AND_LINE_ );
    failedConnectionAttempts.clear();
    completedConnectionAttempts.clear();
    for( Packet* pPacket : headPush )
        DeallocatePacket( pPacket );
    headPush.clear();
    for( Packet* pPacket : tailPush )
        DeallocatePacket( pPacket );
    tailPush.clear();

#if OPEN_SSL_CLIENT_SUPPORT == 1
    SSL_CTX_free( ctx );
    startSSL.Clear( _FILE_AND_LINE_ );
    activeSSLConnections.clear();
#endif

    // Connect is still refused from here on: the update thread is gone.
    std::lock_guard<std::mutex> guard( connectAttemptMutex );
    isStopping = false;
}
bool TCPInterface::BeginConnectAttempt( void )
{
    std::lock_guard<std::mutex> guard( connectAttemptMutex );
    if( isStopping || threadRunning == 0 )
        return false;
    connectAttemptCount++;
    return true;
}
void TCPInterface::EndConnectAttempt( void )
{
    // Notified under the lock: notified after it, Stop could return, and the destructor
    // destroy connectAttemptEnded, before the notify.
    std::lock_guard<std::mutex> guard( connectAttemptMutex );
    connectAttemptCount--;
    connectAttemptEnded.notify_all();
}
bool TCPInterface::IsStopping( void )
{
    std::lock_guard<std::mutex> guard( connectAttemptMutex );
    return isStopping;
}
TCPInterface::ConnectAttempt::ConnectAttempt( TCPInterface& owner )
: tcpInterface( owner )
{
    isBegun = tcpInterface.BeginConnectAttempt();
    isEndPending = isBegun;
}
TCPInterface::ConnectAttempt::~ConnectAttempt()
{
    if( isEndPending )
        tcpInterface.EndConnectAttempt();
}
bool TCPInterface::ConnectAttempt::IsBegun( void ) const
{
    return isBegun;
}
void TCPInterface::ConnectAttempt::HandOn( void )
{
    RakAssert( isEndPending );
    isEndPending = false;
}
void TCPInterface::ReleaseRemoteClient( int index )
{
    std::lock_guard<std::mutex> guard( remoteClients[index].isActiveMutex );
    remoteClients[index].Free();
}
bool TCPInterface::ActivateConnectingClient( int index, __TCPSOCKET__ socket, const SystemAddress& systemAddress )
{
    RemoteClient& remoteClient = remoteClients[index];
    std::lock_guard<std::mutex> guard( remoteClient.isActiveMutex );
    if( remoteClient.state != RemoteClient::State::CONNECTING )
        return false;
    remoteClient.socket = socket;
    remoteClient.systemAddress = systemAddress;
    remoteClient.Activate();
    return true;
}
bool TCPInterface::CloseRemoteClientAt( int index, const SystemAddress& systemAddress )
{
    std::lock_guard<std::mutex> guard( remoteClients[index].isActiveMutex );
    if( remoteClients[index].state != RemoteClient::State::ACTIVE || remoteClients[index].systemAddress != systemAddress )
        return false;
    remoteClients[index].Free();
    return true;
}
void TCPInterface::ReportLostRemoteClientLocked( RemoteClient& remoteClient )
{
    // Freed before the event is queued, so an application that has taken the event from
    // HasLostConnection no longer counts the connection in GetConnectionCount.
    remoteClient.Free();
    SystemAddress* lostConnectionSystemAddress = lostConnections.Allocate( _FILE_AND_LINE_ );
    *lostConnectionSystemAddress = remoteClient.systemAddress;
    lostConnections.Push( lostConnectionSystemAddress );
}
void TCPInterface::CloseRemoteClientOverOutgoingCap( int index )
{
    std::lock_guard<std::mutex> guard( remoteClients[index].isActiveMutex );
    if( remoteClients[index].state != RemoteClient::State::ACTIVE || remoteClients[index].isOverOutgoingCap == false )
        return;
    ReportLostRemoteClientLocked( remoteClients[index] );
}
void TCPInterface::ReleaseIncomingBytes( const Packet& packet )
{
    const SystemIndex index = packet.systemAddress.systemIndex;
    if( index >= remoteClientsLength )
        return;

    // A packet read from a connection since closed is charged to nobody: the entry's count
    // was reset when it was freed. Only a reconnect from the same address into the same
    // entry takes the old connection's bytes as its own, and never below 0.
    RemoteClient& remoteClient = remoteClients[index];
    std::lock_guard<std::mutex> guard( remoteClient.isActiveMutex );
    if( remoteClient.state != RemoteClient::State::ACTIVE || remoteClient.systemAddress != packet.systemAddress )
        return;
    unsigned int queued = remoteClient.incomingBytesQueued;
    while( remoteClient.incomingBytesQueued.compare_exchange_weak( queued, queued > packet.length ? queued - packet.length : 0 ) == false )
    {
    }
}
TCPInterface::RemoteClientSlot::RemoteClientSlot( TCPInterface& owner )
: tcpInterface( owner )
, index( owner.remoteClientsLength )
, isPublished( false )
, isReleasePending( false )
{
    for( int i = 0; i < owner.remoteClientsLength; i++ )
    {
        std::unique_lock<std::mutex> lock( owner.remoteClients[i].isActiveMutex );
        if( owner.remoteClients[i].state == RemoteClient::State::FREE )
        {
            // Keeps the entry's lock, so nothing else can claim it and the caller's writes
            // land before Activate() publishes it. index stops being remoteClientsLength
            // here, which is what IsClaimed reads.
            entryLock = std::move( lock );
            index = i;
            isReleasePending = true;
            return;
        }
    }
}
TCPInterface::RemoteClientSlot::~RemoteClientSlot()
{
    Release();
}
bool TCPInterface::RemoteClientSlot::IsClaimed( void ) const
{
    return index != tcpInterface.remoteClientsLength;
}
int TCPInterface::RemoteClientSlot::GetIndex( void ) const
{
    RakAssert( IsClaimed() );
    return index;
}
RemoteClient& TCPInterface::RemoteClientSlot::Get( void ) const
{
    RakAssert( IsClaimed() );
    return tcpInterface.remoteClients[index];
}
void TCPInterface::RemoteClientSlot::Activate( void )
{
    RakAssert( isReleasePending && isPublished == false );
    tcpInterface.remoteClients[index].Activate();
    isPublished = true;
    entryLock.unlock();
}
void TCPInterface::RemoteClientSlot::Reserve( void )
{
    RakAssert( isReleasePending && isPublished == false );
    tcpInterface.remoteClients[index].Reserve();
    isPublished = true;
    entryLock.unlock();
}
void TCPInterface::RemoteClientSlot::Release( void )
{
    if( isReleasePending == false )
        return;

    isReleasePending = false;

    if( isPublished )
    {
        // Taken and unlocked, so giving it back means taking the lock again.
        tcpInterface.ReleaseRemoteClient( index );
    }
    else
    {
        // Still FREE, so dropping the lock is the whole of the release.
        entryLock.unlock();
    }

    // The entry is anyone's again, so this handle no longer names it: IsClaimed goes
    // false, and Get and GetIndex stop handing out a reference to a slot already given up.
    index = tcpInterface.remoteClientsLength;
}
void TCPInterface::RemoteClientSlot::Commit( void )
{
    // Committing before Activate() or Reserve() would hand on an entry that is still FREE
    // and leave its lock held for good, so it is a caller error rather than a state to
    // recover from. The index is kept: the caller goes on naming the entry it handed on.
    RakAssert( isPublished );
    isReleasePending = false;
}
SystemAddress TCPInterface::Connect( const char* host, unsigned short remotePort, bool block, unsigned short socketFamily, const char* bindAddress )
{
    // The non-blocking arm copies bindAddress into a fixed array, so it has a hard length
    // limit; the blocking arm passes the pointer straight through and has none. Enforcing
    // the smaller of the two here gives Connect one contract rather than one per arm, and
    // rejecting is the honest answer: truncating would bind to an address the caller never
    // asked for. Ahead of the slot claim, so a rejected call claims no remoteClients slot.
    if( bindAddress != 0 && strlen( bindAddress ) > MAXIMUM_BIND_ADDRESS_LENGTH )
        return UNASSIGNED_SYSTEM_ADDRESS;

    // "Table is full" is the handle's answer rather than a loop counter left one past the
    // end of the array, so there is no out-of-range index here to guard against indexing
    // with. What fills the entry in is a connect that can block, and holding the entry's
    // lock across that would stall the update loop, so the entry is reserved now and
    // activated by ActivateConnectingClient once the connect has a socket. Declared after
    // the attempt, so the slot is done with remoteClients before the attempt is counted out.
    ConnectAttempt attempt( *this );
    if( attempt.IsBegun() == false )
        return UNASSIGNED_SYSTEM_ADDRESS;

    RemoteClientSlot slot( *this );
    if( slot.IsClaimed() == false )
        return UNASSIGNED_SYSTEM_ADDRESS;

    slot.Reserve();
    const int newRemoteClientIndex = slot.GetIndex();

    if( block )
    {
        SystemAddress systemAddress;
        systemAddress.FromString( host );
        systemAddress.SetPortHostOrder( remotePort );
        systemAddress.systemIndex = (SystemIndex)newRemoteClientIndex;
        char buffout[128];
        systemAddress.ToString( false, buffout );

        __TCPSOCKET__ sockfd = SocketConnect( buffout, remotePort, socketFamily, bindAddress );
        if( sockfd == INVALID_SOCKET )
        {
            // Released here rather than left to the end of the scope, so the slot is free
            // by the time the application can observe the failure and retry.
            slot.Release();

            failedConnectionAttemptMutex.lock();
            failedConnectionAttempts.push_back( systemAddress );
            failedConnectionAttemptMutex.unlock();

            return UNASSIGNED_SYSTEM_ADDRESS;
        }

        // From here on the entry is ActivateConnectingClient's to fill in, and then the
        // connection's until CloseConnection or the update loop gives it back.
        slot.Commit();

        if( ActivateConnectingClient( newRemoteClientIndex, sockfd, systemAddress ) == false )
        {
            closesocket__( sockfd );

            failedConnectionAttemptMutex.lock();
            failedConnectionAttempts.push_back( systemAddress );
            failedConnectionAttemptMutex.unlock();

            return UNASSIGNED_SYSTEM_ADDRESS;
        }

        completedConnectionAttemptMutex.lock();
        completedConnectionAttempts.push_back( systemAddress );
        completedConnectionAttemptMutex.unlock();

        return systemAddress;
    }
    else
    {
        ThisPtrPlusSysAddr* s = RakNet::OP_NEW<ThisPtrPlusSysAddr>( _FILE_AND_LINE_ );
        s->systemAddress.FromStringExplicitPort( host, remotePort );
        s->systemAddress.systemIndex = (SystemIndex)newRemoteClientIndex;
        // Bounded by the length check at the top of Connect.
        if( bindAddress )
            strcpy( s->bindAddress, bindAddress );
        else
            s->bindAddress[0] = 0;
        s->tcpInterface = this;
        s->socketFamily = socketFamily;

        // Handed on before the thread starts: the thread may end the attempt at once, and
        // then Stop may free the slot and return, so nothing here may touch either after.
        slot.Commit();
        attempt.HandOn();

        // Start the connection thread
        const int errorCode = RakThread::Create( ConnectionAttemptLoop, s, threadPriority );

        if( errorCode != 0 )
        {
            SystemAddress failedSystemAddress = s->systemAddress;
            RakNet::OP_DELETE( s, _FILE_AND_LINE_ );

            // No thread was started, so nothing else will ever clear the slot. Released
            // before the failure is pushed, so the slot is free by the time the
            // application can observe the failure and retry.
            ReleaseRemoteClient( newRemoteClientIndex );

            failedConnectionAttemptMutex.lock();
            failedConnectionAttempts.push_back( failedSystemAddress );
            failedConnectionAttemptMutex.unlock();

            EndConnectAttempt();
        }
        return UNASSIGNED_SYSTEM_ADDRESS;
    }
}
#if OPEN_SSL_CLIENT_SUPPORT == 1
void TCPInterface::StartSSLClient( SystemAddress systemAddress )
{
    if( ctx == 0 )
    {
        std::lock_guard<std::mutex> guard( sharedSslMutex );
        SSLeay_add_ssl_algorithms();
        meth = (SSL_METHOD*)SSLv23_client_method();
        SSL_load_error_strings();
        ctx = SSL_CTX_new( meth );
        RakAssert( ctx != 0 );
    }

    SystemAddress* id = startSSL.Allocate( _FILE_AND_LINE_ );
    *id = systemAddress;
    startSSL.Push( id );

    auto it = std::find( activeSSLConnections.begin(), activeSSLConnections.end(), systemAddress );
    if( it == activeSSLConnections.end() )
    {
        activeSSLConnections.emplace_back( systemAddress );
    }
}
bool TCPInterface::IsSSLActive( SystemAddress systemAddress )
{
    return std::find( activeSSLConnections.begin(), activeSSLConnections.end(), systemAddress ) != activeSSLConnections.end();
}
#endif
void TCPInterface::Send( const char* data, unsigned int length, const SystemAddress& systemAddress, bool broadcast )
{
    SendList( &data, &length, 1, systemAddress, broadcast );
}
bool TCPInterface::SendList( const char** data, const unsigned int* lengths, const int numParameters, const SystemAddress& systemAddress, bool broadcast )
{
    if( isStarted == 0 )
        return false;
    if( data == 0 )
        return false;
    if( systemAddress == UNASSIGNED_SYSTEM_ADDRESS && broadcast == false )
        return false;
    unsigned int totalLength = 0;
    int i;
    for( i = 0; i < numParameters; i++ )
    {
        if( lengths[i] > 0 )
            totalLength += lengths[i];
    }
    if( totalLength == 0 )
        return false;

    const unsigned int maxOutgoingBytes = maxOutgoingBytesPerClient;
    // Buffers for the entry if it is active and its address is a target, testing both under
    // its lock. Returns whether it was a target.
    auto bufferFor = [&]( RemoteClient& remoteClient ) {
        std::lock_guard<std::mutex> guard( remoteClient.isActiveMutex );
        if( remoteClient.state != RemoteClient::State::ACTIVE )
            return false;
        if( ( remoteClient.systemAddress == systemAddress ) == broadcast )
            return false;
        if( remoteClient.SendOrBuffer( data, lengths, numParameters, maxOutgoingBytes ) == false )
            return true;

        // Once per interface: a flood of closes should not flood the console too.
        if( outgoingBytesCapCloseCount.fetch_add( 1 ) == 0 )
        {
            RAKNET_DEBUG_PRINTF( "TCPInterface: closing a connection with %u bytes waiting to be sent to it (SetMaxOutgoingBytesPerClient). See GetOutgoingBytesCapCloseCount.\n", maxOutgoingBytes );
        }
        return true;
    };

    // Broadcast sends to all but systemAddress. Otherwise the entry systemIndex names is
    // tried first, and the search is for when it does not name the connection.
    if( broadcast || systemAddress.systemIndex >= remoteClientsLength || bufferFor( remoteClients[systemAddress.systemIndex] ) == false )
    {
        for( i = 0; i < remoteClientsLength; i++ )
            bufferFor( remoteClients[i] );
    }


    return true;
}
void TCPInterface::SetMaxIncomingBytesPerClient( unsigned int maxBytes )
{
    maxIncomingBytesPerClient = maxBytes == 0 ? 1 : maxBytes;
}
unsigned int TCPInterface::GetMaxIncomingBytesPerClient( void ) const
{
    return maxIncomingBytesPerClient;
}
void TCPInterface::SetMaxOutgoingBytesPerClient( unsigned int maxBytes )
{
    maxOutgoingBytesPerClient = maxBytes;
}
unsigned int TCPInterface::GetMaxOutgoingBytesPerClient( void ) const
{
    return maxOutgoingBytesPerClient;
}
uint64_t TCPInterface::GetIncomingBytesCapStallCount( void ) const
{
    return incomingBytesCapStallCount;
}
uint64_t TCPInterface::GetOutgoingBytesCapCloseCount( void ) const
{
    return outgoingBytesCapCloseCount;
}
bool TCPInterface::ReceiveHasPackets( void )
{
    return !headPush.empty() || incomingMessages.IsEmpty() == false || !tailPush.empty();
}
Packet* TCPInterface::Receive( void )
{
    for( PluginInterface2* pPlugin : messageHandlerList )
    {
        pPlugin->Update();
    }

    Packet* outgoingPacket = ReceiveInt();

    if( outgoingPacket )
    {
        for( PluginInterface2* pPlugin : messageHandlerList )
        {
            PluginReceiveResult pluginResult = pPlugin->OnReceive( outgoingPacket );
            if( pluginResult == RR_STOP_PROCESSING_AND_DEALLOCATE )
            {
                DeallocatePacket( outgoingPacket );
                outgoingPacket = 0; // Will do the loop again and get another packet
                break;              // break out of the enclosing for
            }
            else if( pluginResult == RR_STOP_PROCESSING )
            {
                outgoingPacket = 0;
                break;
            }
        }
    }

    return outgoingPacket;
}
Packet* TCPInterface::ReceiveInt( void )
{
    if( isStarted == 0 )
        return 0;
    if( !headPush.empty() )
    {
        Packet* p = headPush.front();
        headPush.pop_front();
        return p;
    }
    Packet* p = incomingMessages.Pop();
    if( p )
    {
        ReleaseIncomingBytes( *p );
        return p;
    }
    if( !tailPush.empty() )
    {
        Packet* p = tailPush.front();
        tailPush.pop_front();
        return p;
    }
    return 0;
}


void TCPInterface::AttachPlugin( PluginInterface2* plugin )
{
    if( std::find( messageHandlerList.begin(), messageHandlerList.end(), plugin ) == messageHandlerList.end() )
    {
        messageHandlerList.push_back( plugin );
        plugin->SetTCPInterface( this );
        plugin->OnAttach();
    }
}
void TCPInterface::DetachPlugin( PluginInterface2* plugin )
{
    if( plugin == 0 )
        return;

    auto it = std::find( messageHandlerList.begin(), messageHandlerList.end(), plugin );
    if( it != messageHandlerList.end() )
    {
        (*it)->OnDetach();
        // Unordered list so delete from end for speed
        messageHandlerList.erase( it );
        plugin->SetTCPInterface( 0 );
    }
}
bool TCPInterface::CloseConnection( SystemAddress systemAddress )
{
    return CloseConnection( systemAddress, LCR_CLOSED_BY_USER );
}
bool TCPInterface::CloseConnection( const SystemAddress& systemAddress, PI2_LostConnectionReason reason )
{
    if( isStarted == 0 )
        return false;
    if( systemAddress == UNASSIGNED_SYSTEM_ADDRESS )
        return false;

    for( PluginInterface2* pPlugin : messageHandlerList )
    {
        pPlugin->OnClosedConnection( systemAddress, UNASSIGNED_RAKNET_GUID, reason );
    }

    // The fast path tries the entry systemIndex names. The search is for when it does not
    // name the connection - it is stale, unset, or out of range - so nothing in there may
    // index with it. The entry to close is the one the loop matches, i, which is in range
    // by construction. Each test and release is one step under the entry's lock, so an
    // entry the update loop has reported lost is never closed, or reported, a second time.
    bool isClosed = false;
    if( systemAddress.systemIndex < remoteClientsLength )
        isClosed = CloseRemoteClientAt( systemAddress.systemIndex, systemAddress );
    for( int i = 0; isClosed == false && i < remoteClientsLength; i++ )
        isClosed = CloseRemoteClientAt( i, systemAddress );


#if OPEN_SSL_CLIENT_SUPPORT == 1
    auto it = std::find( activeSSLConnections.begin(), activeSSLConnections.end(), systemAddress );
    if( it != activeSSLConnections.end() )
    {
        activeSSLConnections.erase( it );
    }
#endif

    return isClosed;
}
void TCPInterface::DeallocatePacket( Packet* packet )
{
    if( packet == 0 )
        return;
    if( packet->deleteData )
    {
        rakFree_Ex( packet->data, _FILE_AND_LINE_ );
        incomingMessages.Deallocate( packet, _FILE_AND_LINE_ );
    }
    else
    {
        // Came from userspace AllocatePacket
        rakFree_Ex( packet->data, _FILE_AND_LINE_ );
        RakNet::OP_DELETE( packet, _FILE_AND_LINE_ );
    }
}
Packet* TCPInterface::AllocatePacket( unsigned dataSize )
{
    unsigned char* data = (unsigned char*)rakMalloc_Ex( dataSize, _FILE_AND_LINE_ );
    if( data == 0 && dataSize != 0 )
    {
        notifyOutOfMemory( _FILE_AND_LINE_ );
        return 0;
    }
    Packet* p = RakNet::OP_NEW<Packet>( _FILE_AND_LINE_ );
    p->data = data;
    p->length = dataSize;
    p->bitSize = BYTES_TO_BITS( dataSize );
    p->deleteData = false;
    p->guid = UNASSIGNED_RAKNET_GUID;
    p->systemAddress = UNASSIGNED_SYSTEM_ADDRESS;
    p->systemAddress.systemIndex = (SystemIndex)-1;
    return p;
}
void TCPInterface::PushBackPacket( Packet* packet, bool pushAtHead )
{
    if( pushAtHead )
        headPush.push_back( packet );
    else
        tailPush.push_back( packet );
}
bool TCPInterface::WasStarted( void ) const
{
    return threadRunning > 0;
}
SystemAddress TCPInterface::HasCompletedConnectionAttempt( void )
{
    SystemAddress sysAddr = UNASSIGNED_SYSTEM_ADDRESS;
    completedConnectionAttemptMutex.lock();
    if( !completedConnectionAttempts.empty() )
    {
        sysAddr = completedConnectionAttempts.front();
        completedConnectionAttempts.pop_front();
    }
    completedConnectionAttemptMutex.unlock();

    if( sysAddr != UNASSIGNED_SYSTEM_ADDRESS )
    {
        for( PluginInterface2* pPlugin : messageHandlerList )
        {
            pPlugin->OnNewConnection( sysAddr, UNASSIGNED_RAKNET_GUID, true );
        }
    }

    return sysAddr;
}
SystemAddress TCPInterface::HasFailedConnectionAttempt( void )
{
    SystemAddress sysAddr = UNASSIGNED_SYSTEM_ADDRESS;
    failedConnectionAttemptMutex.lock();
    if( !failedConnectionAttempts.empty() )
    {
        sysAddr = failedConnectionAttempts.front();
        failedConnectionAttempts.pop_front();
    }
    failedConnectionAttemptMutex.unlock();

    if( sysAddr != UNASSIGNED_SYSTEM_ADDRESS )
    {
        for( PluginInterface2* pPlugin : messageHandlerList )
        {
            Packet p;
            p.systemAddress = sysAddr;
            p.data = 0;
            p.length = 0;
            p.bitSize = 0;
            pPlugin->OnFailedConnectionAttempt( &p, FCAR_CONNECTION_ATTEMPT_FAILED );
        }
    }

    return sysAddr;
}
SystemAddress TCPInterface::HasNewIncomingConnection( void )
{
    SystemAddress* out = newIncomingConnections.Pop();
    if( out )
    {
        SystemAddress out2 = *out;
        newIncomingConnections.Deallocate( out, _FILE_AND_LINE_ );

        for( PluginInterface2* pPlugin : messageHandlerList )
        {
            pPlugin->OnNewConnection( out2, UNASSIGNED_RAKNET_GUID, true );
        }

        return out2;
    }
    else
    {
        return UNASSIGNED_SYSTEM_ADDRESS;
    }
}
SystemAddress TCPInterface::HasLostConnection( void )
{
    SystemAddress* out = lostConnections.Pop();
    if( out )
    {
        SystemAddress out2 = *out;
        lostConnections.Deallocate( out, _FILE_AND_LINE_ );

        for( PluginInterface2* pPlugin : messageHandlerList )
        {
            pPlugin->OnClosedConnection( out2, UNASSIGNED_RAKNET_GUID, LCR_DISCONNECTION_NOTIFICATION );
        }

        return out2;
    }
    else
    {
        return UNASSIGNED_SYSTEM_ADDRESS;
    }
}
void TCPInterface::GetConnectionList( SystemAddress* remoteSystems, unsigned short* numberOfSystems ) const
{
    unsigned short systemCount = 0;
    unsigned short maxToWrite = *numberOfSystems;
    for( int i = 0; i < remoteClientsLength; i++ )
    {
        std::lock_guard<std::mutex> guard( remoteClients[i].isActiveMutex );
        if( remoteClients[i].state == RemoteClient::State::ACTIVE )
        {
            if( systemCount < maxToWrite )
                remoteSystems[systemCount] = remoteClients[i].systemAddress;
            systemCount++;
        }
    }
    *numberOfSystems = systemCount;
}
unsigned short TCPInterface::GetConnectionCount( void ) const
{
    unsigned short systemCount = 0;
    for( int i = 0; i < remoteClientsLength; i++ )
    {
        std::lock_guard<std::mutex> guard( remoteClients[i].isActiveMutex );
        if( remoteClients[i].state == RemoteClient::State::ACTIVE )
            systemCount++;
    }
    return systemCount;
}

unsigned int TCPInterface::GetOutgoingDataBufferSize( SystemAddress systemAddress ) const
{
    // The bytes buffered for the entry at index if it is active at systemAddress, or
    // nothing if it is not.
    auto bytesFor = [&]( int index, bool& isMatched ) -> unsigned int {
        RemoteClient& remoteClient = remoteClients[index];
        std::lock_guard<std::mutex> guard( remoteClient.isActiveMutex );
        isMatched = remoteClient.state == RemoteClient::State::ACTIVE && remoteClient.systemAddress == systemAddress;
        if( isMatched == false )
            return 0;
        std::lock_guard<std::mutex> outgoingGuard( remoteClient.outgoingDataMutex );
        return (unsigned int)remoteClient.outgoingData.Size();
    };

    bool isMatched = false;
    if( systemAddress.systemIndex < remoteClientsLength )
    {
        unsigned int bytesWritten = bytesFor( systemAddress.systemIndex, isMatched );
        if( isMatched )
            return bytesWritten;
    }

    unsigned int bytesWritten = 0;
    for( int i = 0; i < remoteClientsLength; i++ )
        bytesWritten += bytesFor( i, isMatched );
    return bytesWritten;
}
__TCPSOCKET__ TCPInterface::SocketConnect( const char* host, unsigned short remotePort, unsigned short socketFamily, const char* bindAddress )
{
#if RAKNET_SUPPORT_IPV6 != 1
    (void)socketFamily;

    sockaddr_in serverAddress;

    // getaddrinfo is thread-safe, and connect attempts resolve concurrently.
    struct addrinfo hints, *res = 0;
    memset( &hints, 0, sizeof hints );
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if( getaddrinfo( host, 0, &hints, &res ) != 0 )
        return INVALID_SOCKET;
    const in_addr serverHostAddress = ( (const sockaddr_in*)res->ai_addr )->sin_addr;
    freeaddrinfo( res );

    __TCPSOCKET__ sockfd = socket__( AF_INET, SOCK_STREAM, 0 );
    if( sockfd == INVALID_SOCKET )
        return INVALID_SOCKET;

    memset( &serverAddress, 0, sizeof( serverAddress ) );
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons( remotePort );

    if( bindAddress && bindAddress[0] )
    {
        serverAddress.sin_addr.s_addr = inet_addr__( bindAddress );
    }
    else
    {
        serverAddress.sin_addr.s_addr = INADDR_ANY;
    }

    int sock_opt = 1024 * 256;
    setsockopt__( sockfd, SOL_SOCKET, SO_RCVBUF, (char*)&sock_opt, sizeof( sock_opt ) );

    serverAddress.sin_addr = serverHostAddress;

    const bool isConnected = ConnectUnlessStopping( sockfd, (struct sockaddr*)&serverAddress, sizeof( struct sockaddr ) );

#else

    (void)bindAddress;

    struct addrinfo hints, *res = 0;
    __TCPSOCKET__ sockfd;
    memset( &hints, 0, sizeof hints );
    hints.ai_family = socketFamily;
    hints.ai_socktype = SOCK_STREAM;
    char portStr[32];
    auto portRes = std::to_chars( portStr, portStr + 31, remotePort );
    RakAssert( portRes.ec == std::errc() );
    *portRes.ptr = '\0';

    if( getaddrinfo( host, portStr, &hints, &res ) != 0 )
        return INVALID_SOCKET;

    sockfd = socket__( res->ai_family, res->ai_socktype, res->ai_protocol );
    if( sockfd == INVALID_SOCKET )
    {
        freeaddrinfo( res );
        return INVALID_SOCKET;
    }
    const bool isConnected = ConnectUnlessStopping( sockfd, res->ai_addr, (int)res->ai_addrlen );
    freeaddrinfo( res ); // free the linked-list

#endif // #if RAKNET_SUPPORT_IPV6!=1

    if( isConnected == false )
    {
        closesocket__( sockfd );
        return INVALID_SOCKET;
    }

    return sockfd;
}
bool TCPInterface::ConnectUnlessStopping( __TCPSOCKET__ sockfd, const sockaddr* address, int addressLength )
{
    // Long enough not to spin, short enough that Stop never waits noticeably for it.
    constexpr long kStopCheckMicroseconds = 50000;

    if( SetSocketBlocking( sockfd, false ) == false )
        return false;

    if( connect__( sockfd, address, addressLength ) != 0 )
    {
        if( IsConnectInProgress() == false )
            return false;

        for( ;; )
        {
            if( IsStopping() )
                return false;

            fd_set writeFD, exceptionFD;
            FD_ZERO( &writeFD );
            FD_ZERO( &exceptionFD );
            FD_SET( sockfd, &writeFD );
            // Winsock reports a failed connect here rather than as writable.
            FD_SET( sockfd, &exceptionFD );
            timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = kStopCheckMicroseconds;

            const int selectResult = select__( (int)sockfd + 1, 0, &writeFD, &exceptionFD, &tv );
            if( selectResult > 0 )
                break;
            if( selectResult < 0 && IsInterruptedCall() == false )
                return false;
        }

        int connectError = 0;
        socklen_t connectErrorLength = sizeof( connectError );
        if( getsockopt__( sockfd, SOL_SOCKET, SO_ERROR, (char*)&connectError, &connectErrorLength ) != 0 || connectError != 0 )
            return false;
    }

    // The update loop and RemoteClient::Send rely on blocking sends.
    return SetSocketBlocking( sockfd, true );
}

void ConnectionAttemptLoop( void* arg )
{
    TCPInterface::ThisPtrPlusSysAddr* s = (TCPInterface::ThisPtrPlusSysAddr*)arg;

    SystemAddress systemAddress = s->systemAddress;
    TCPInterface* tcpInterface = s->tcpInterface;
    int newRemoteClientIndex = systemAddress.systemIndex;
    unsigned short socketFamily = s->socketFamily;
    // Sized from the source array rather than repeating its extent, and that array was
    // bounded by Connect before the thread was started, so this copy cannot overrun.
    char bindAddress[sizeof( s->bindAddress )];
    strcpy( bindAddress, s->bindAddress );
    RakNet::OP_DELETE( s, _FILE_AND_LINE_ );

    char str1[64];
    systemAddress.ToString( false, str1 );
    __TCPSOCKET__ sockfd = tcpInterface->SocketConnect( str1, systemAddress.GetPort(), socketFamily, bindAddress );
    if( sockfd == INVALID_SOCKET )
    {
        tcpInterface->ReleaseRemoteClient( newRemoteClientIndex );

        std::lock_guard<std::mutex> guard( tcpInterface->failedConnectionAttemptMutex );
        tcpInterface->failedConnectionAttempts.push_back( systemAddress );
    }
    else if( tcpInterface->ActivateConnectingClient( newRemoteClientIndex, sockfd, systemAddress ) == false )
    {
        closesocket__( sockfd );

        std::lock_guard<std::mutex> guard( tcpInterface->failedConnectionAttemptMutex );
        tcpInterface->failedConnectionAttempts.push_back( systemAddress );
    }
    else
    {
        std::lock_guard<std::mutex> guard( tcpInterface->completedConnectionAttemptMutex );
        tcpInterface->completedConnectionAttempts.push_back( systemAddress );
    }

    // Last: once the attempt is counted out, Stop may free everything above and return.
    tcpInterface->EndConnectAttempt();
}

void UpdateTCPInterfaceLoop( void* arg )
{
    TCPInterface* sts = (TCPInterface*)arg;

    //  const int BUFF_SIZE=8096;
    const unsigned int BUFF_SIZE = 1048576;
    //char data[ BUFF_SIZE ];
    char* data = (char*)rakMalloc_Ex( BUFF_SIZE, _FILE_AND_LINE_ );
    Packet* incomingMessage;
    fd_set readFD, exceptionFD, writeFD;
    sts->threadRunning++;

#if RAKNET_SUPPORT_IPV6 != 1
    sockaddr_in sockAddr;
    int sockAddrSize = sizeof( sockAddr );
#else
    struct sockaddr_storage sockAddr;
    socklen_t sockAddrSize = sizeof( sockAddr );
#endif

    int len;
    __TCPSOCKET__ newSock;
    int selectResult;


    timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 30000;

    // Each entry's generation when its socket went into the select sets, or 0 if it did not.
    // remoteClientsLength is fixed while this thread runs.
    std::vector<uint32_t> selectedGenerations( sts->remoteClientsLength, 0 );

    while( sts->isStarted > 0 )
    {
#if OPEN_SSL_CLIENT_SUPPORT == 1
        SystemAddress* sslSystemAddress;
        sslSystemAddress = sts->startSSL.Pop();
        if( sslSystemAddress )
        {
            // Starts SSL on the entry at index if it is active at the address and has none.
            auto initSSLAt = [&]( int index ) {
                RemoteClient& remoteClient = sts->remoteClients[index];
                std::lock_guard<std::mutex> guard( remoteClient.isActiveMutex );
                if( remoteClient.state != RemoteClient::State::ACTIVE || remoteClient.systemAddress != *sslSystemAddress )
                    return false;
                if( remoteClient.ssl == 0 )
                    remoteClient.InitSSL( sts->ctx, sts->meth );
                return true;
            };
            if( sslSystemAddress->systemIndex >= sts->remoteClientsLength || initSSLAt( sslSystemAddress->systemIndex ) == false )
            {
                for( int i = 0; i < sts->remoteClientsLength; i++ )
                    initSSLAt( i );
            }
            sts->startSSL.Deallocate( sslSystemAddress, _FILE_AND_LINE_ );
        }
#endif


        __TCPSOCKET__ largestDescriptor = 0; // see select__()'s first parameter's documentation under linux


        // Linux' select__() implementation changes the timeout

        tv.tv_sec = 0;
        tv.tv_usec = 30000;

        // On Linux, Stop's shutdown leaves the listen socket readable, so select keeps
        // succeeding; isStarted is what ends this loop then.
        while( sts->isStarted > 0 )
        {
            // Clients SendOrBuffer flagged at the outgoing cap. Not left to the select below,
            // which a client that does not read may never make writable.
            for( int i = 0; i < sts->remoteClientsLength; i++ )
            {
                if( sts->remoteClients[i].isOverOutgoingCap )
                    sts->CloseRemoteClientOverOutgoingCap( i );
            }

            // Reset readFD, writeFD, and exceptionFD since select seems to clear it
            FD_ZERO( &readFD );
            FD_ZERO( &exceptionFD );
            FD_ZERO( &writeFD );
            largestDescriptor = 0;
            if( sts->listenSocket != INVALID_SOCKET )
            {
                FD_SET( sts->listenSocket, &readFD );
                FD_SET( sts->listenSocket, &exceptionFD );
                largestDescriptor = sts->listenSocket; // @see largestDescriptor def
            }

            for( int i = 0; i < sts->remoteClientsLength; i++ )
            {
                RemoteClient& remoteClient = sts->remoteClients[i];
                std::lock_guard<std::mutex> guard( remoteClient.isActiveMutex );
                if( remoteClient.state != RemoteClient::State::ACTIVE )
                {
                    selectedGenerations[i] = 0;
                    continue;
                }

                // An ACTIVE entry always has a socket: Connect's entries are activated only
                // once their connect has one.
                selectedGenerations[i] = remoteClient.generation;
                // At the incoming cap the socket is not read, so TCP flow control holds the
                // client back until Receive drains what it sent.
                if( remoteClient.incomingBytesQueued < sts->maxIncomingBytesPerClient )
                    FD_SET( remoteClient.socket, &readFD );
                FD_SET( remoteClient.socket, &exceptionFD );
                bool hasOutgoingData;
                {
                    std::lock_guard<std::mutex> outgoingGuard( remoteClient.outgoingDataMutex );
                    hasOutgoingData = remoteClient.outgoingData.Size() > 0;
                }
                if( hasOutgoingData )
                    FD_SET( remoteClient.socket, &writeFD );
                if( remoteClient.socket > largestDescriptor ) // @see largestDescriptorDef
                    largestDescriptor = remoteClient.socket;
            }

            selectResult = (int)select__( largestDescriptor + 1, &readFD, &writeFD, &exceptionFD, &tv );

            if( selectResult <= 0 )
                break;

            if( sts->listenSocket != INVALID_SOCKET && FD_ISSET( sts->listenSocket, &readFD ) )
            {
                newSock = accept__( sts->listenSocket, (sockaddr*)&sockAddr, (socklen_t*)&sockAddrSize );

                // INVALID_SOCKET is a failed accept, which Stop shutting down the listen socket
                // under select produces. Stored, it would reach FD_SET, which aborts on it under glibc.
                if( newSock != INVALID_SOCKET )
                {
                    // "Table is full" is the handle's answer, so there is no index one past
                    // the end of the array in play here. The writes below happen while the
                    // handle still holds the entry's lock, so the entry is never visible as
                    // active with its socket and address unset - the ordering the old
                    // hand-rolled lock/unlock had, kept rather than argued about.
                    TCPInterface::RemoteClientSlot slot( *sts );
                    if( slot.IsClaimed() )
                    {
                        RemoteClient& newRemoteClient = slot.Get();
                        newRemoteClient.socket = newSock;

#if RAKNET_SUPPORT_IPV6 != 1
                        newRemoteClient.systemAddress.address.addr4.sin_addr.s_addr = sockAddr.sin_addr.s_addr;
                        newRemoteClient.systemAddress.SetPortNetworkOrder( sockAddr.sin_port );
#else
                        if( sockAddr.ss_family == AF_INET )
                        {
                            memcpy( &newRemoteClient.systemAddress.address.addr4, (sockaddr_in*)&sockAddr, sizeof( sockaddr_in ) );
                            //  newRemoteClient.systemAddress.address.addr4.sin_port=ntohs( newRemoteClient.systemAddress.address.addr4.sin_port );
                        }
                        else
                        {
                            memcpy( &newRemoteClient.systemAddress.address.addr6, (sockaddr_in6*)&sockAddr, sizeof( sockaddr_in6 ) );
                            //  newRemoteClient.systemAddress.address.addr6.sin6_port=ntohs( newRemoteClient.systemAddress.address.addr6.sin6_port );
                        }

#endif // #if RAKNET_SUPPORT_IPV6!=1
                        // Both families: Receive finds the entry a packet's bytes are
                        // charged to by this index.
                        newRemoteClient.systemAddress.systemIndex = (SystemIndex)slot.GetIndex();
                        const SystemAddress newSystemAddress = newRemoteClient.systemAddress;
                        slot.Activate();

                        // The entry belongs to the connection now; this loop gives it back
                        // when the connection is lost, not when this scope ends.
                        slot.Commit();

                        SystemAddress* newConnectionSystemAddress = sts->newIncomingConnections.Allocate( _FILE_AND_LINE_ );
                        *newConnectionSystemAddress = newSystemAddress;
                        sts->newIncomingConnections.Push( newConnectionSystemAddress );
                    }
                    else
                    {
                        // Nowhere to put it. Close the connection we just accepted, not the
                        // listen socket: the interface has to go on accepting, so that a
                        // slot freeing up is all it takes to serve the next one.
                        closesocket__( newSock );
                    }
                }
                else
                {
#ifdef _DO_PRINTF
                    RAKNET_DEBUG_PRINTF( "Error: connection failed\n" );
#endif
                }
            }
            else if( sts->listenSocket != INVALID_SOCKET && FD_ISSET( sts->listenSocket, &exceptionFD ) )
            {
#ifdef _DO_PRINTF
                int err;
                int errlen = sizeof( err );
                getsockopt__( sts->listenSocket, SOL_SOCKET, SO_ERROR, (char*)&err, &errlen );
                RAKNET_DEBUG_PRINTF( "Socket error %s on listening socket\n", err );
#endif
            }

            // Each entry's work is done under its lock, so CloseConnection cannot free it, nor
            // a connect fill it in, part way through. An entry whose generation has changed
            // since select holds another connection, or none, and select's answer is not
            // about it.
            for( int i = 0; i < sts->remoteClientsLength; i++ )
            {
                RemoteClient& remoteClient = sts->remoteClients[i];
                std::lock_guard<std::mutex> guard( remoteClient.isActiveMutex );
                if( selectedGenerations[i] == 0 || remoteClient.state != RemoteClient::State::ACTIVE || remoteClient.generation != selectedGenerations[i] )
                    continue;

                if( FD_ISSET( remoteClient.socket, &exceptionFD ) )
                {
                    // Connection lost abruptly.
                    sts->ReportLostRemoteClientLocked( remoteClient );
                    continue;
                }

                // Read no more than takes the client to the incoming cap, which may have been
                // lowered since select.
                const unsigned int maxIncomingBytes = sts->maxIncomingBytesPerClient;
                const unsigned int incomingBytesQueued = remoteClient.incomingBytesQueued;
                const unsigned int incomingRoom = incomingBytesQueued < maxIncomingBytes ? ( std::min )( maxIncomingBytes - incomingBytesQueued, BUFF_SIZE ) : 0;
                if( FD_ISSET( remoteClient.socket, &readFD ) && incomingRoom > 0 )
                {
                    // if recv returns 0 this was a graceful close
                    len = remoteClient.Recv( data, (int)incomingRoom );

                    if( len > 0 )
                    {
                        incomingMessage = sts->incomingMessages.Allocate( _FILE_AND_LINE_ );
                        incomingMessage->data = (unsigned char*)rakMalloc_Ex( len + 1, _FILE_AND_LINE_ );
                        memcpy( incomingMessage->data, data, len );
                        incomingMessage->data[len] = 0; // Null terminate this so we can print it out as regular strings.  This is different from RakNet which does not do this.
                        incomingMessage->length = len;
                        incomingMessage->deleteData = true; // actually means came from SPSC, rather than AllocatePacket
                        incomingMessage->systemAddress = remoteClient.systemAddress;

                        // Charged before it can be received, so Receive never gives back
                        // bytes that were not yet counted.
                        if( remoteClient.incomingBytesQueued.fetch_add( (unsigned int)len ) + (unsigned int)len >= maxIncomingBytes )
                        {
                            // Once per interface: every slow poll would print otherwise.
                            if( sts->incomingBytesCapStallCount.fetch_add( 1 ) == 0 )
                            {
                                RAKNET_DEBUG_PRINTF( "TCPInterface: stopped reading a client with %u bytes from it waiting for Receive (SetMaxIncomingBytesPerClient). See GetIncomingBytesCapStallCount.\n", maxIncomingBytes );
                            }
                        }
                        sts->incomingMessages.Push( incomingMessage );
                    }
                    else
                    {
                        // Connection lost gracefully.
                        sts->ReportLostRemoteClientLocked( remoteClient );
                        continue;
                    }
                }
                if( FD_ISSET( remoteClient.socket, &writeFD ) )
                {
                    std::lock_guard<std::mutex> outgoingGuard( remoteClient.outgoingDataMutex );
                    size_t bytesInBuffer = 0;
                    const char* bytesToSend = remoteClient.outgoingData.Contiguous( &bytesInBuffer );
                    if( bytesInBuffer > 0 )
                    {
                        // send and SSL_write take an int length.
                        const unsigned int bytesToSendLength = bytesInBuffer < BUFF_SIZE ? (unsigned int)bytesInBuffer : BUFF_SIZE;
                        const int bytesSent = remoteClient.Send( bytesToSend, bytesToSendLength );
                        if( bytesSent > 0 )
                            remoteClient.outgoingData.Consume( bytesSent );
                    }
                }
            }
        }

        // Sleep 0 on Linux monopolizes the CPU
        std::this_thread::sleep_for( std::chrono::milliseconds( 30 ) );
    }

    // Freed before threadRunning drops: once it does, Stop may return and the caller may
    // swap the allocator hooks.
    rakFree_Ex( data, _FILE_AND_LINE_ );

    sts->threadRunning--;
}

void RemoteClient::Reserve( void )
{
    RakAssert( state == State::FREE );
    state = State::CONNECTING;
}
void RemoteClient::Activate( void )
{
    RakAssert( state != State::ACTIVE && socket != INVALID_SOCKET );
    state = State::ACTIVE;
    Reset();
    if( ++generation == 0 )
        generation = 1;
}
void RemoteClient::Free( void )
{
    if( state == State::FREE )
        return;
    state = State::FREE;
    Reset();
#if OPEN_SSL_CLIENT_SUPPORT == 1
    FreeSSL();
#endif
    if( socket != INVALID_SOCKET )
    {
        closesocket__( socket );
        socket = INVALID_SOCKET;
    }
}
bool RemoteClient::SendOrBuffer( const char** data, const unsigned int* lengths, const int numParameters, unsigned int maxOutgoingBytes )
{
    if( state != State::ACTIVE )
        return false;

    uint64_t totalLength = 0;
    for( int parameterIndex = 0; parameterIndex < numParameters; parameterIndex++ )
        totalLength += lengths[parameterIndex];

    // All or nothing, under one lock, so the check and the writes agree.
    std::lock_guard<std::mutex> guard( outgoingDataMutex );
    if( isOverOutgoingCap )
        return false;
    if( outgoingData.Size() + totalLength > maxOutgoingBytes )
    {
        // Buffered no further: the far end is not reading. The update loop closes it.
        isOverOutgoingCap = true;
        return true;
    }
    for( int parameterIndex = 0; parameterIndex < numParameters; parameterIndex++ )
        outgoingData.Append( data[parameterIndex], lengths[parameterIndex] );
    return false;
}
#if OPEN_SSL_CLIENT_SUPPORT == 1
bool RemoteClient::InitSSL( SSL_CTX* ctx, SSL_METHOD* meth )
{
    (void)meth;

    ssl = SSL_new( ctx );
    RakAssert( ssl );
    int res;
    res = SSL_set_fd( ssl, socket );
    if( res != 1 )
    {
        printf( "SSL_set_fd error: %s\n", ERR_reason_error_string( ERR_get_error() ) );
        SSL_free( ssl );
        ssl = 0;
        return false;
    }
    RakAssert( res == 1 );
    res = SSL_connect( ssl );
    if( res < 0 )
    {
        unsigned long err = ERR_get_error();
        printf( "SSL_connect error: %s\n", ERR_reason_error_string( err ) );
        SSL_free( ssl );
        ssl = 0;
        return false;
    }
    else if( res == 0 )
    {
        // The TLS/SSL handshake was not successful but was shut down controlled and by the specifications of the TLS/SSL protocol. Call SSL_get_error() with the return value ret to find out the reason.
        int err = SSL_get_error( ssl, res );
        switch( err )
        {
        case SSL_ERROR_NONE:
            printf( "SSL_ERROR_NONE\n" );
            break;
        case SSL_ERROR_ZERO_RETURN:
            printf( "SSL_ERROR_ZERO_RETURN\n" );
            break;
        case SSL_ERROR_WANT_READ:
            printf( "SSL_ERROR_WANT_READ\n" );
            break;
        case SSL_ERROR_WANT_WRITE:
            printf( "SSL_ERROR_WANT_WRITE\n" );
            break;
        case SSL_ERROR_WANT_CONNECT:
            printf( "SSL_ERROR_WANT_CONNECT\n" );
            break;
        case SSL_ERROR_WANT_ACCEPT:
            printf( "SSL_ERROR_WANT_ACCEPT\n" );
            break;
        case SSL_ERROR_WANT_X509_LOOKUP:
            printf( "SSL_ERROR_WANT_X509_LOOKUP\n" );
            break;
        case SSL_ERROR_SYSCALL: {
            // http://www.openssl.org/docs/ssl/SSL_get_error.html
            char buff[1024];
            unsigned long ege = ERR_get_error();
            if( ege == 0 && res == 0 )
                printf( "SSL_ERROR_SYSCALL EOF in violation of the protocol\n" );
            else if( ege == 0 && res == -1 )
                printf( "SSL_ERROR_SYSCALL %s\n", strerror( errno ) );
            else
                printf( "SSL_ERROR_SYSCALL %s\n", ERR_error_string( ege, buff ) );
        }
        break;
        case SSL_ERROR_SSL:
            printf( "SSL_ERROR_SSL\n" );
            break;
        }
    }

    if( res != 1 )
    {
        SSL_free( ssl );
        ssl = 0;
        return false;
    }
    return true;
}
void RemoteClient::DisconnectSSL( void )
{
    if( ssl )
        SSL_shutdown( ssl ); /* send SSL/TLS close_notify */
}
void RemoteClient::FreeSSL( void )
{
    if( ssl )
        SSL_free( ssl );
    ssl = 0;
}
int RemoteClient::Send( const char* data, unsigned int length )
{
    if( ssl )
        return SSL_write( ssl, data, length );
    else
        return send__( socket, data, length, 0 );
}
int RemoteClient::Recv( char* data, const int dataSize )
{
    if( ssl )
        return SSL_read( ssl, data, dataSize );
    else
        return recv__( socket, data, dataSize, 0 );
}
#else
int RemoteClient::Send( const char* data, unsigned int length )
{
    return send__( socket, data, length, 0 );
}
int RemoteClient::Recv( char* data, const int dataSize )
{
    return recv__( socket, data, dataSize, 0 );
}
#endif

} // namespace RakNet

#endif // _RAKNET_SUPPORT_*
