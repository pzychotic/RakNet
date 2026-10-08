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
#if _RAKNET_SUPPORT_PacketizedTCP == 1 && _RAKNET_SUPPORT_TCPInterface == 1

#include "TCPInterface.h"
#include "TCPByteBuffer.h"
#include "MTUSize.h"

#include <cstdint>
#include <deque>
#include <map>

namespace RakNet {

class RAK_DLL_EXPORT PacketizedTCP : public TCPInterface
{
public:
    // GetInstance() and DestroyInstance(instance*)
    STATIC_FACTORY_DECLARATIONS( PacketizedTCP )

    PacketizedTCP();
    virtual ~PacketizedTCP();

    /// Stops the TCP server
    void Stop( void );

    /// Sends a byte stream
    void Send( const char* data, unsigned length, const SystemAddress& systemAddress, bool broadcast );

    // Sends a concatenated list of byte streams
    bool SendList( const char** data, const unsigned int* lengths, const int numParameters, const SystemAddress& systemAddress, bool broadcast );

    /// Returns data received
    Packet* Receive( void );

    /// Disconnects a player/address
    /// \return As TCPInterface::CloseConnection.
    bool CloseConnection( SystemAddress systemAddress );

    /// Has a previous call to connect succeeded?
    /// \return UNASSIGNED_SYSTEM_ADDRESS = no. Anything else means yes.
    SystemAddress HasCompletedConnectionAttempt( void );

    /// Has a previous call to connect failed?
    /// \return UNASSIGNED_SYSTEM_ADDRESS = no. Anything else means yes.
    SystemAddress HasFailedConnectionAttempt( void );

    /// Queued events of new incoming connections
    SystemAddress HasNewIncomingConnection( void );

    /// Queued events of lost connections. Also reports a connection closed at
    /// SetMaxMessageLength's cap.
    SystemAddress HasLostConnection( void );

    /// The longest message a connection may announce. Its header is read before the message
    /// is buffered, so a header announcing more closes the connection at once: the stream
    /// cannot be re-framed after skipping a frame. The close is reported by
    /// HasLostConnection. So a connection holds at most about this much while a message
    /// arrives, where the 32-bit header let it make this interface buffer up to 4 GiB.
    /// Default MAXIMUM_MESSAGE_SIZE, what RakPeer delivers; may be changed while started.
    /// Counted by GetMessageLengthCapCloseCount. The incoming and outgoing byte caps of
    /// TCPInterface apply as well.
    void SetMaxMessageLength( unsigned int maxLength );
    unsigned int GetMaxMessageLength( void ) const;

    /// How many connections SetMaxMessageLength's cap closed.
    uint64_t GetMessageLengthCapCloseCount( void ) const;

protected:
    /// What is buffered for one remote address.
    struct Connection
    {
        /// Bytes received and not yet framed into a message.
        TCPByteBuffer bytes;

        /// Connections at this address reported new and not yet lost. A reconnect from the
        /// same address can be reported new before the old connection is reported lost, so
        /// the entry goes only when this reaches 0. It can go below 0 if a lost event is
        /// drained first.
        int openConnections = 0;

        /// Closed at the message length cap: what is still queued from it is dropped.
        bool isClosed = false;
    };

    void ClearAllConnections( void );

    /// Adds \a delta to the entry for \a sa, creating it if missing and deleting it at 0.
    /// A new connection (delta 1) starts a new stream, so the entry's bytes are discarded.
    /// Received data carries only an address, so bytes the old connection sent that are
    /// still queued behind the reconnect's new event are framed as the reconnect's: a
    /// reconnect from the same source port can find its stream starting mid-message.
    void CountConnection( const SystemAddress& sa, int delta );

    /// Appends bytes read from \a sa to its entry and frames every complete message out of
    /// them onto waitingPackets. May close the connection, and so delete \a connection.
    void FrameMessages( const Packet& incomingPacket, Connection& connection );

    /// Closes the connection at \a sa for announcing a message longer than the maximum.
    /// May delete \a connection.
    void CloseOverlongSender( const SystemAddress& sa, Connection& connection );

    void PushNotificationsToQueues( void );
    Packet* ReturnOutgoingPacket( void );

    // A single TCP recieve may generate multiple split packets. They are stored in the waitingPackets list until Receive is called
    std::deque<Packet*> waitingPackets;
    std::map<SystemAddress, Connection*> connections;

    unsigned int maxMessageLength;
    uint64_t messageLengthCapCloseCount;

    // Mirrors single producer / consumer, but processes them in Receive() before returning to user
    std::deque<SystemAddress> _newIncomingConnections, _lostConnections, _failedConnectionAttempts, _completedConnectionAttempts;
};

} // namespace RakNet

#endif // _RAKNET_SUPPORT_*
