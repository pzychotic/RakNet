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
/// \brief Contains RakNetTransportCommandParser and RakNetTransport used to provide a secure console connection.
///

#pragma once

#include "NativeFeatureIncludes.h"
#if _RAKNET_SUPPORT_TelnetTransport == 1

#include "TransportInterface.h"
#include "CommandParserInterface.h"
#include "PluginInterface2.h"
#include "Export.h"

#include <deque>

namespace RakNet {

/// \defgroup RAKNET_TRANSPORT_GROUP RakNetTransport
/// \brief UDP based transport implementation for the ConsoleServer
/// \details
/// \ingroup PLUGINS_GROUP

/// \brief Use RakNetTransport if you need a secure connection between the client and the console server.
/// \details RakNetTransport automatically initializes security for the system.  Use the project CommandConsoleClient to connect
/// To the ConsoleServer if you use RakNetTransport
/// \ingroup RAKNET_TRANSPORT_GROUP
class RAK_DLL_EXPORT RakNetTransport2 : public TransportInterface, public PluginInterface2
{
public:
    // GetInstance() and DestroyInstance(instance*)
    STATIC_FACTORY_DECLARATIONS( RakNetTransport2 )

    RakNetTransport2();
    ~RakNetTransport2() override;

    /// Start the transport provider on the indicated port.
    /// \param[in] port The port to start the transport provider on
    /// \param[in] serverMode If true, you should allow incoming connections (I don't actually use this anywhere)
    /// \return Return true on success, false on failure.
    [[nodiscard]] bool Start( unsigned short port, bool serverMode ) override;

    /// Stop the transport provider.  You can clear memory and shutdown threads here.
    void Stop( void ) override;

    /// Send a null-terminated string to \a systemAddress
    /// If your transport method requires particular formatting of the outgoing data (e.g. you don't just send strings) you can do it here
    /// and parse it out in Receive().
    /// \param[in] systemAddress The player to send the string to
    /// \param[in] data format specifier - same as RAKNET_DEBUG_PRINTF
    /// \param[in] ... format specification arguments - same as RAKNET_DEBUG_PRINTF
    void Send( SystemAddress systemAddress, const char* data, ... ) override;

    /// Disconnect \a systemAddress .  The binary address and port defines the SystemAddress structure.
    /// \param[in] systemAddress The player/address to disconnect
    void CloseConnection( SystemAddress systemAddress ) override;

    /// Return a string. The string should be allocated and written to Packet::data .
    /// The byte length should be written to Packet::length .  The player/address should be written to Packet::systemAddress
    /// If your transport protocol adds special formatting to the data stream you should parse it out before returning it in the packet
    /// and thus only return a string in Packet::data
    /// \return The packet structure containing the result of Receive, or 0 if no data is available
    Packet* Receive( void ) override;

    /// Deallocate the Packet structure returned by Receive
    /// \param[in] The packet to deallocate
    void DeallocatePacket( Packet* packet ) override;

    /// If a new system connects to you, you should queue that event and return the systemAddress/address of that player in this function.
    /// \return The SystemAddress/address of the system
    SystemAddress HasNewIncomingConnection( void ) override;

    /// If a system loses the connection, you should queue that event and return the systemAddress/address of that player in this function.
    /// \return The SystemAddress/address of the system
    SystemAddress HasLostConnection( void ) override;

    CommandParserInterface* GetCommandParser( void ) override { return 0; }

    /// \internal
    PluginReceiveResult OnReceive( Packet* packet ) override;
    /// \internal
    void OnClosedConnection( const SystemAddress& systemAddress, RakNetGUID rakNetGUID, PI2_LostConnectionReason lostConnectionReason ) override;
    /// \internal
    void OnNewConnection( const SystemAddress& systemAddress, RakNetGUID rakNetGUID, bool isIncoming ) override;

protected:

    std::deque<SystemAddress> newConnections, lostConnections;
    std::deque<Packet*> packetQueue;
};

} // namespace RakNet

#endif // _RAKNET_SUPPORT_*
