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
#include "DS_ByteQueue.h"

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
    void CloseConnection( SystemAddress systemAddress );

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

protected:
    void ClearAllConnections( void );
    void RemoveFromConnectionList( const SystemAddress& sa );
    void AddToConnectionList( const SystemAddress& sa );
    void PushNotificationsToQueues( void );
    Packet* ReturnOutgoingPacket( void );

    // A single TCP recieve may generate multiple split packets. They are stored in the waitingPackets list until Receive is called
    std::deque<Packet*> waitingPackets;
    std::map<SystemAddress, DataStructures::ByteQueue*> connections;

    // Mirrors single producer / consumer, but processes them in Receive() before returning to user
    std::deque<SystemAddress> _newIncomingConnections, _lostConnections, _failedConnectionAttempts, _completedConnectionAttempts;
};

} // namespace RakNet

#endif // _RAKNET_SUPPORT_*
