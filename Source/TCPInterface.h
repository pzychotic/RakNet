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
/// \brief A simple TCP based server allowing sends and receives.  Can be connected by any TCP client, including telnet.
///

#pragma once

#include "NativeFeatureIncludes.h"
#if _RAKNET_SUPPORT_TCPInterface == 1

#include "RakMemoryOverride.h"
#include "RakNetTypes.h"
#include "Export.h"
#include "RakNetDefines.h"
#include "SocketIncludes.h"
#include "DS_ByteQueue.h"
#include "DS_ThreadsafeAllocatingQueue.h"
#include "MTUSize.h"
#include "PluginInterface2.h"

#if OPEN_SSL_CLIENT_SUPPORT == 1
#include <openssl/crypto.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace RakNet {

/// Forward declarations
struct RemoteClient;

/// \internal
/// \brief As the name says, a simple multithreaded TCP server.  Used by TelnetTransport
class RAK_DLL_EXPORT TCPInterface
{
public:
    // GetInstance() and DestroyInstance(instance*)
    STATIC_FACTORY_DECLARATIONS( TCPInterface )

    TCPInterface();
    virtual ~TCPInterface();

    /// Longest bindAddress Connect() will take, not counting the terminator. The array it is
    /// copied into is sized from this, so the two cannot drift apart; public because it is
    /// part of Connect()'s contract and callers have no other way to see the array.
    static constexpr size_t MAXIMUM_BIND_ADDRESS_LENGTH = 63;

    // TODO - add socketdescriptor
    /// Starts the TCP server on the indicated port
    /// \param[in] port Which port to listen on.
    /// \param[in] maxIncomingConnections Max incoming connections we will accept
    /// \param[in] maxConnections Max total connections, which should be >= maxIncomingConnections
    /// \param[in] threadPriority Passed to the thread creation routine. Use THREAD_PRIORITY_NORMAL for Windows. For Linux based systems, you MUST pass something reasonable based on the thread priorities for your application.
    /// \param[in] socketFamily IP version: For IPV4, use AF_INET (default). For IPV6, use AF_INET6. To autoselect, use AF_UNSPEC.
    bool Start( unsigned short port, unsigned short maxIncomingConnections, unsigned short maxConnections = 0, int _threadPriority = -99999, unsigned short socketFamily = AF_INET, const char* bindAddress = 0 );

    /// Stops the TCP server. Waits for every connect attempt to end, and ends each one still
    /// connecting within about 50 ms. An attempt still resolving its host name is the
    /// exception: resolution can't be interrupted, so Stop may wait for the resolver's own
    /// timeout.
    void Stop( void );

    /// Connect to the specified host on the specified port. Refused, with
    /// UNASSIGNED_SYSTEM_ADDRESS and no failed connection attempt reported, if the interface
    /// is not started or Stop is running.
    /// \param[in] bindAddress Local address to bind to, or 0 for none. At most
    /// MAXIMUM_BIND_ADDRESS_LENGTH characters: a longer one is rejected with
    /// UNASSIGNED_SYSTEM_ADDRESS rather than truncated.
    SystemAddress Connect( const char* host, unsigned short remotePort, bool block = true, unsigned short socketFamily = AF_INET, const char* bindAddress = 0 );

#if OPEN_SSL_CLIENT_SUPPORT == 1
    /// Start SSL on an existing connection, notified with HasCompletedConnectionAttempt
    void StartSSLClient( SystemAddress systemAddress );

    /// Was SSL started on this socket?
    bool IsSSLActive( SystemAddress systemAddress );
#endif

    /// Sends a byte stream
    virtual void Send( const char* data, unsigned int length, const SystemAddress& systemAddress, bool broadcast );

    // Sends a concatenated list of byte streams
    virtual bool SendList( const char** data, const unsigned int* lengths, const int numParameters, const SystemAddress& systemAddress, bool broadcast );

    // Get how many bytes are waiting to be sent. If too many, you may want to skip sending
    unsigned int GetOutgoingDataBufferSize( SystemAddress systemAddress ) const;

    /// Default for SetMaxIncomingBytesPerClient.
    static constexpr unsigned int DEFAULT_MAXIMUM_INCOMING_BYTES_PER_CLIENT = 1024 * 1024;

    /// Default for SetMaxOutgoingBytesPerClient: one largest RakNet Message and room beside
    /// it, so PacketizedTCP can send anything RakPeer could.
    static constexpr unsigned int DEFAULT_MAXIMUM_OUTGOING_BYTES_PER_CLIENT = 2 * MAXIMUM_MESSAGE_SIZE;

    /// The most bytes received from one client that may wait for Receive. At the cap the
    /// receive thread stops reading that client's socket until Receive drains it below the
    /// cap, so TCP flow control slows the sender. Nothing is dropped and nothing is closed.
    /// So a client cannot make this interface hold more than the cap for it however slowly
    /// the application polls, and every client's cap times maxConnections bounds the whole.
    /// Reading resumes on the receive thread's next pass, up to about 60 ms after the drain,
    /// so a client held at the cap gets about one cap per pass through. A stalled client's
    /// socket is not read at all, so its disconnect too is seen only after the drain. Default
    /// DEFAULT_MAXIMUM_INCOMING_BYTES_PER_CLIENT; 0 is taken as 1. May be changed while
    /// started. Counted by GetIncomingBytesCapStallCount.
    void SetMaxIncomingBytesPerClient( unsigned int maxBytes );
    unsigned int GetMaxIncomingBytesPerClient( void ) const;

    /// The most bytes Send may buffer for one client, waiting for it to read them. A Send
    /// that would pass the cap closes the connection instead, since the far end is not
    /// reading: that Send and every later one to it are discarded, and the connection is
    /// reported by HasLostConnection. A single Send larger than the cap closes it too, so a
    /// cap of 0 closes every connection on its next Send.
    /// Default DEFAULT_MAXIMUM_OUTGOING_BYTES_PER_CLIENT; may be changed while started.
    /// Counted by GetOutgoingBytesCapCloseCount.
    void SetMaxOutgoingBytesPerClient( unsigned int maxBytes );
    unsigned int GetMaxOutgoingBytesPerClient( void ) const;

    /// How many times a client's reads were paused at SetMaxIncomingBytesPerClient's cap.
    uint64_t GetIncomingBytesCapStallCount( void ) const;

    /// How many connections SetMaxOutgoingBytesPerClient's cap closed.
    uint64_t GetOutgoingBytesCapCloseCount( void ) const;

    /// Returns if Receive() will return data
    /// Do not use on PacketizedTCP
    virtual bool ReceiveHasPackets( void );

    /// Returns data received. Draining it is what lets the receive thread read a client past
    /// SetMaxIncomingBytesPerClient's cap again. Connection events wait in their own queues,
    /// which only the Has... calls drain, so an application polls those every tick too.
    virtual Packet* Receive( void );

    /// Disconnects a player/address
    /// \return True if this call closed an open connection. False if there was none at
    /// \a systemAddress, including one whose loss was already detected: that one is, or
    /// will be, reported by HasLostConnection instead. No lost event follows a true.
    bool CloseConnection( SystemAddress systemAddress );

    /// Deallocates a packet returned by Receive
    void DeallocatePacket( Packet* packet );

    /// Fills the array remoteSystems with the SystemAddress of all the systems we are connected to
    /// \param[out] remoteSystems An array of SystemAddress structures to be filled with the SystemAddresss of the systems we are connected to. Pass 0 to remoteSystems to only get the number of systems we are connected to
    /// \param[in, out] numberOfSystems As input, the size of remoteSystems array.  As output, the number of elements put into the array
    void GetConnectionList( SystemAddress* remoteSystems, unsigned short* numberOfSystems ) const;

    /// Returns just the number of connections we have
    unsigned short GetConnectionCount( void ) const;

    /// Has a previous call to connect succeeded?
    /// \return UNASSIGNED_SYSTEM_ADDRESS = no. Anything else means yes.
    SystemAddress HasCompletedConnectionAttempt( void );

    /// Has a previous call to connect failed?
    /// \return UNASSIGNED_SYSTEM_ADDRESS = no. Anything else means yes.
    SystemAddress HasFailedConnectionAttempt( void );

    /// Queued events of new incoming connections
    SystemAddress HasNewIncomingConnection( void );

    /// Queued events of lost connections
    SystemAddress HasLostConnection( void );

    /// Return an allocated but empty packet, for custom use, or 0 if memory ran out
    Packet* AllocatePacket( unsigned dataSize );

    // Push a packet back to the queue
    virtual void PushBackPacket( Packet* packet, bool pushAtHead );

    /// Returns if Start() was called successfully
    bool WasStarted( void ) const;

    void AttachPlugin( PluginInterface2* plugin );
    void DetachPlugin( PluginInterface2* plugin );

protected:
    Packet* ReceiveInt( void );

    bool CreateListenSocket( unsigned short port, unsigned short maxIncomingConnections, unsigned short socketFamily, const char* hostAddress );

    // Plugins
    std::vector<PluginInterface2*> messageHandlerList;

    std::atomic<uint32_t> isStarted, threadRunning;
    __TCPSOCKET__ listenSocket;

    std::deque<Packet*> headPush, tailPush;
    RemoteClient* remoteClients;
    int remoteClientsLength;

    DataStructures::ThreadsafeAllocatingQueue<Packet> incomingMessages;
    DataStructures::ThreadsafeAllocatingQueue<SystemAddress> newIncomingConnections, lostConnections, requestedCloseConnections;
    DataStructures::ThreadsafeAllocatingQueue<RemoteClient*> newRemoteClients;
    std::mutex completedConnectionAttemptMutex, failedConnectionAttemptMutex;
    std::deque<SystemAddress> completedConnectionAttempts, failedConnectionAttempts;

    int threadPriority;

    std::atomic<unsigned int> maxIncomingBytesPerClient, maxOutgoingBytesPerClient;
    std::atomic<uint64_t> incomingBytesCapStallCount, outgoingBytesCapCloseCount;

    /// CloseConnection, telling plugins \a reason: a close at a cap is a lost connection to
    /// them, not one the application closed.
    bool CloseConnection( const SystemAddress& systemAddress, PI2_LostConnectionReason reason );

    /// Queues \a remoteClient's lost event and frees it. Its isActiveMutex is held.
    void ReportLostRemoteClientLocked( RemoteClient& remoteClient );

    /// Gives back the incoming bytes \a packet held against its client's cap, if the client
    /// it came from is still the one in its entry. Called as Receive hands a packet on.
    void ReleaseIncomingBytes( const Packet& packet );

    /// The update loop's close for a client flagged at the outgoing cap: queues its lost
    /// event and frees the entry, under its isActiveMutex, if it is still active and flagged.
    void CloseRemoteClientOverOutgoingCap( int index );

    /// Guards isStopping and connectAttemptCount; connectAttemptEnded is notified under it.
    std::mutex connectAttemptMutex;
    std::condition_variable connectAttemptEnded;

    /// Set for the length of Stop. Connect refuses, and connects in flight give up.
    bool isStopping;

    /// Connect attempts begun and not yet ended. Stop frees nothing until it is zero.
    unsigned int connectAttemptCount;

    /// Counts a connect attempt in, unless the update thread is not running or Stop is.
    /// Returns whether it did.
    bool BeginConnectAttempt( void );

    /// Counts a connect attempt out. It is the attempt's last touch of this object: once it
    /// returns, Stop may return and the object be destroyed.
    void EndConnectAttempt( void );

    /// Whether Stop is running, read under connectAttemptMutex.
    bool IsStopping( void );

    friend void UpdateTCPInterfaceLoop( void* arg );
    friend void ConnectionAttemptLoop( void* arg );

    /// Resolves \a host and connects to it. Returns the connected socket, in blocking mode,
    /// or INVALID_SOCKET if the connect failed or Stop asked it to give up.
    __TCPSOCKET__ SocketConnect( const char* host, unsigned short remotePort, unsigned short socketFamily, const char* bindAddress );

    /// Connects \a sockfd to \a address without blocking, waiting for the result in short
    /// steps so it can give up as soon as Stop is running. Returns whether it connected.
    bool ConnectUnlessStopping( __TCPSOCKET__ sockfd, const sockaddr* address, int addressLength );

    /// \internal
    /// \brief A scoped count on one connect attempt.
    ///
    /// Construction is BeginConnectAttempt. Unless the count is handed on to the thread that
    /// carries the attempt on, which then ends it itself, the destructor ends it.
    class ConnectAttempt
    {
    public:
        explicit ConnectAttempt( TCPInterface& owner );
        ~ConnectAttempt();

        ConnectAttempt( const ConnectAttempt& ) = delete;
        ConnectAttempt& operator=( const ConnectAttempt& ) = delete;

        /// Whether the attempt was counted in. False means Connect is refused.
        bool IsBegun( void ) const;

        /// Hands the count on: whoever it was handed to calls EndConnectAttempt.
        void HandOn( void );

    private:
        TCPInterface& tcpInterface;
        bool isBegun;
        bool isEndPending;
    };

    struct ThisPtrPlusSysAddr
    {
        TCPInterface* tcpInterface;
        SystemAddress systemAddress;
        bool useSSL;
        char bindAddress[MAXIMUM_BIND_ADDRESS_LENGTH + 1];
        unsigned short socketFamily;
    };

    /// The one lock / Free / unlock that gives back the entry at \a index, connecting or
    /// active. Called by RemoteClientSlot and by the connect thread, which releases a slot
    /// claimed elsewhere and so holds an index rather than a handle. The update loop and
    /// CloseConnection free a connection through ReportLostRemoteClientLocked and
    /// CloseRemoteClientAt instead.
    void ReleaseRemoteClient( int index );

    /// Finishes a connect into the entry at \a index, which Connect reserved: under its
    /// isActiveMutex, writes \a socket and \a systemAddress and makes the entry active, so
    /// it is never active with either unset. Returns false, writing nothing, if the entry
    /// is not connecting any more; the caller then still owns \a socket and closes it.
    bool ActivateConnectingClient( int index, __TCPSOCKET__ socket, const SystemAddress& systemAddress );

    /// CloseConnection's release: frees the entry at \a index if it is active at
    /// \a systemAddress, testing and freeing under its isActiveMutex. Returns whether it
    /// freed it.
    bool CloseRemoteClientAt( int index, const SystemAddress& systemAddress );

    /// \internal
    /// \brief A scoped claim on one remoteClients entry.
    ///
    /// Construction is the one walk of the array that finds a free entry, and it leaves
    /// that entry's isActiveMutex held. Unless the claim is committed, the destructor
    /// gives the entry back, which is what makes a claimed-but-never-handed-on slot
    /// unrepresentable rather than something each claiming site has to remember not to
    /// leak.
    ///
    /// The claim ends in one of two states:
    ///
    /// - Activate(): the accept path writes .socket and .systemAddress through Get() while
    ///   the handle still holds the lock, then activates the entry.
    /// - Reserve(): Connect marks the entry connecting and drops the lock, since what fills
    ///   it in is a connect that can block for as long as the network takes. A connecting
    ///   entry is nobody's connection: only ActivateConnectingClient or a release touches
    ///   it.
    class RemoteClientSlot
    {
    public:
        /// Claims \a owner's first free entry, or nothing at all if the table is full.
        explicit RemoteClientSlot( TCPInterface& owner );

        /// Gives the entry back unless the claim was committed.
        ~RemoteClientSlot();

        RemoteClientSlot( const RemoteClientSlot& ) = delete;
        RemoteClientSlot& operator=( const RemoteClientSlot& ) = delete;

        /// Whether this handle holds an entry. False is the "table is full" answer,
        /// carried by the handle rather than by a loop counter's terminal value, and it is
        /// also what a released handle reports. Nothing below may be called when it is
        /// false; a committed handle still reports true, because the entry it names goes
        /// on being the caller's to read.
        bool IsClaimed( void ) const;

        /// Index of the held entry, which is what SystemAddress::systemIndex carries.
        int GetIndex( void ) const;

        /// The held entry. Before Activate() this handle holds its isActiveMutex, so
        /// writes made through here are published before the entry becomes active. Only
        /// valid while the lock is held.
        RemoteClient& Get( void ) const;

        /// Marks the entry active and drops its lock.
        void Activate( void );

        /// Marks the entry connecting and drops its lock.
        void Reserve( void );

        /// Gives the entry back now rather than at the end of the scope, for callers that
        /// want the slot free before they publish a failure the application can retry on.
        /// The handle holds nothing afterwards.
        void Release( void );

        /// Hands the claim on: the entry stays taken past this scope, and whoever it was
        /// handed to is the one that releases it. Only valid after Activate() or Reserve().
        void Commit( void );

    private:
        TCPInterface& tcpInterface;
        std::unique_lock<std::mutex> entryLock;

        /// remoteClientsLength when this handle holds no entry.
        int index;

        /// Whether Activate() or Reserve() has dropped the entry's lock.
        bool isPublished;

        /// Whether this handle is still the one that owes the entry a release.
        bool isReleasePending;
    };

#if OPEN_SSL_CLIENT_SUPPORT == 1
    SSL_CTX* ctx;
    SSL_METHOD* meth;
    DataStructures::ThreadsafeAllocatingQueue<SystemAddress> startSSL;
    std::vector<SystemAddress> activeSSLConnections;
    std::mutex sharedSslMutex;
#endif
};

/// Stores information about a remote client.
///
/// Every field that is not atomic is read and written under isActiveMutex, and outgoingData
/// under outgoingDataMutex as well. A thread that needs both takes isActiveMutex first.
struct RemoteClient
{
    /// FREE entries are claimed by RemoteClientSlot. CONNECTING ones belong to a Connect in
    /// flight and are no connection yet: only ACTIVE ones are counted, listed, sent to,
    /// closed or selected on.
    enum class State
    {
        FREE,
        CONNECTING,
        ACTIVE
    };

    RemoteClient()
    {
#if OPEN_SSL_CLIENT_SUPPORT == 1
        ssl = 0;
#endif
        state = State::FREE;
        generation = 0;
        socket = INVALID_SOCKET;
        incomingBytesQueued = 0;
        isOverOutgoingCap = false;
    }
    __TCPSOCKET__ socket;
    SystemAddress systemAddress;
    DataStructures::ByteQueue outgoingData;
    State state;

    /// Bumped by every Activate, and never 0 while ACTIVE. The update loop keeps it from
    /// select to the work on the result, so a connection that took the entry in between is
    /// not given the old one's readiness, even on a reused descriptor.
    uint32_t generation;

    /// Bytes read from this client and not yet handed on by Receive. Only the receive
    /// thread adds to it, so a read never takes it past the cap.
    std::atomic<unsigned int> incomingBytesQueued;

    /// Set by SendOrBuffer at the outgoing cap; the update loop then closes the connection.
    /// Written under outgoingDataMutex.
    std::atomic<bool> isOverOutgoingCap;
    std::mutex outgoingDataMutex;
    std::mutex isActiveMutex;

#if OPEN_SSL_CLIENT_SUPPORT == 1
    SSL* ssl;
    bool InitSSL( SSL_CTX* ctx, SSL_METHOD* meth );
    void DisconnectSSL( void );
    void FreeSSL( void );
    int Send( const char* data, unsigned int length );
    int Recv( char* data, const int dataSize );
#else
    int Send( const char* data, unsigned int length );
    int Recv( char* data, const int dataSize );
#endif
    void Reset( void )
    {
        std::lock_guard<std::mutex> guard( outgoingDataMutex );
        outgoingData.Clear( _FILE_AND_LINE_ );
        isOverOutgoingCap = false;
        incomingBytesQueued = 0;
    }

    /// Marks the entry connecting. It must be FREE.
    void Reserve( void );

    /// Marks the entry ACTIVE with fresh buffers and a new generation. socket and
    /// systemAddress must already be written.
    void Activate( void );

    /// Gives the entry back from CONNECTING or ACTIVE, closing its socket and freeing its SSL.
    void Free( void );

    /// Buffers the data for the update loop to send, unless that would take outgoingData
    /// past \a maxOutgoingBytes. Then it buffers nothing, flags the client for the update
    /// loop to close, and returns true, once per connection.
    bool SendOrBuffer( const char** data, const unsigned int* lengths, const int numParameters, unsigned int maxOutgoingBytes );
};

} // namespace RakNet

#endif // _RAKNET_SUPPORT_*
