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
/// \brief Contains the NAT-type detection code for the client
///

#pragma once

#include "NativeFeatureIncludes.h"
#if _RAKNET_SUPPORT_NatTypeDetectionClient == 1

#include "Plugins/NatTypeDetectionCommon.h"
#include "RakNetTypes.h"
#include "Export.h"
#include "PluginInterface2.h"
#include "SocketIncludes.h"

#include <atomic>
#include <deque>
#include <mutex>

namespace RakNet {

/// Forward declarations
struct Packet;

/// \brief Client code for NatTypeDetection
/// \details See NatTypeDetectionServer.h for algorithm
/// To use, just connect to the server, and call DetectNAT
/// You will get back ID_NAT_TYPE_DETECTION_RESULT with one of the enumerated values of NATTypeDetectionResult found in NATTypeDetectionCommon.h
/// IPv4 only.
/// See also http://www.jenkinssoftware.com/raknet/manual/natpunchthrough.html
/// \sa NatPunchthroughClient
/// \sa NatTypeDetectionServer
/// \ingroup NAT_TYPE_DETECTION_GROUP
class RAK_DLL_EXPORT NatTypeDetectionClient : public PluginInterface2, public RNS2EventHandler
{
public:
    // GetInstance() and DestroyInstance(instance*)
    STATIC_FACTORY_DECLARATIONS( NatTypeDetectionClient )

    // Constructor
    NatTypeDetectionClient();

    // Destructor
    virtual ~NatTypeDetectionClient();

    /// Send the message to the server to detect the nat type
    /// Server must be running NatTypeDetectionServer
    /// We must already be connected to the server
    /// If the socket it needs for the test fails to bind, it sends nothing and pushes
    /// ID_NAT_TYPE_DETECTION_RESULT with NAT_TYPE_UNKNOWN.
    /// \param[in] serverAddress address of the server
    void DetectNATType( SystemAddress _serverAddress );

    /// \internal For plugin handling
    virtual void Update( void );

    /// \internal For plugin handling
    virtual PluginReceiveResult OnReceive( Packet* packet );

    virtual void OnClosedConnection( const SystemAddress& systemAddress, RakNetGUID rakNetGUID, PI2_LostConnectionReason lostConnectionReason );
    virtual void OnRakPeerShutdown( void );
    virtual void OnDetach( void );

    virtual void OnRNS2Recv( RNS2RecvStruct* recvStruct );
    virtual void DeallocRNS2RecvStruct( RNS2RecvStruct* s, const char* file, unsigned int line );
    virtual RNS2RecvStruct* AllocRNS2RecvStruct( const char* file, unsigned int line );

    /// \brief How many datagrams to the plugin's own socket were dropped because MAX_BUFFERED_RECEIVED_DATAGRAMS already waited for Update.
    /// \details Any sender can reach that socket. The cap is the core's, overridable per build in RakNetDefines.h; it defaults to 8192.
    uint64_t GetReceivedDatagramsDroppedAtCap( void ) const;

protected:
    std::deque<RNS2RecvStruct*> bufferedPackets;
    std::mutex bufferedPacketsMutex;
    std::atomic<uint64_t> receivedDatagramsDroppedAtCap;

    RakNetSocket2* c2;

    /// Creates c2, the socket the server sends its NAT_TYPE_NONE test to. Returns 0 if it fails
    /// to bind. By default an ephemeral port on the same host as the peer's first socket.
    virtual RakNetSocket2* CreateC2Socket( void );

    void Shutdown( void );
    void OnCompletion( NATTypeDetectionResult result );
    bool IsInProgress( void ) const;

    void OnTestPortRestricted( Packet* packet );
    SystemAddress serverAddress;
};


} // namespace RakNet

#endif // _RAKNET_SUPPORT_*
