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
/// \brief Contains the NAT-type detection code for the server
///

#pragma once

#include "NativeFeatureIncludes.h"
#if _RAKNET_SUPPORT_NatTypeDetectionServer == 1

#include "Plugins/NatTypeDetectionCommon.h"
#include "RakNetTypes.h"
#include "Export.h"
#include "PluginInterface2.h"
#include "SocketIncludes.h"

#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace RakNet {

/// Forward declarations
struct Packet;

/// \brief Server code for NatTypeDetection
/// \details
/// Sends to a remote system on certain ports and addresses to determine what type of router, if any, that client is behind
/// Requires that the server have 4 external IP addresses
/// <OL>
/// <LI>Server has 1 instance of RakNet. Server has four external ip addresses S1 to S4. Five ports are used in total P1 to P5. RakNet is bound to S1P1. Sockets are bound to S1P2, S2P3, S3P4, S4P5
/// <LI>Client with one port using RakNet (C1). Another port not using anything (C2).
/// <LI>C1 connects to S1P1 for normal communication.
/// <LI>S4P5 sends to C2. If arrived, no NAT. Done. (If didn't arrive, S4P5 potentially banned, do not use again).
/// <LI>S2P3 sends to C1 (Different address, different port, to previously used port on client). If received, Full-cone nat. Done.  (If didn't arrive, S2P3 potentially banned, do not use again).
/// <LI>S1P2 sends to C1 (Same address, different port, to previously used port on client). If received, address-restricted cone nat. Done.
/// <LI>Server via RakNet connection tells C1 to send to to S3P4. If address of C1 as seen by S3P4 is the same as the address of C1 as seen by S1P1 (RakNet connection), then port-restricted cone nat. Done
/// <LI>Else symmetric nat. Done.
/// </OL>
/// See also http://www.jenkinssoftware.com/raknet/manual/natpunchthrough.html
/// \sa NatPunchthroughServer
/// \sa NatTypeDetectionClient
/// \ingroup NAT_TYPE_DETECTION_GROUP
class RAK_DLL_EXPORT NatTypeDetectionServer : public PluginInterface2, public RNS2EventHandler
{
public:
    // GetInstance() and DestroyInstance(instance*)
    STATIC_FACTORY_DECLARATIONS( NatTypeDetectionServer )

    // Constructor
    NatTypeDetectionServer();

    // Destructor
    virtual ~NatTypeDetectionServer();

    /// Start the system, binding to 3 external IPs not already in useS
    /// \param[in] nonRakNetIP2 First unused external IP
    /// \param[in] nonRakNetIP3 Second unused external IP
    /// \param[in] nonRakNetIP4 Third unused external IP
    void Startup( const char* nonRakNetIP2, const char* nonRakNetIP3, const char* nonRakNetIP4 );

    // Releases the sockets created in Startup();
    void Shutdown( void );

    /// \internal For plugin handling
    virtual void Update( void );

    /// \internal For plugin handling
    virtual PluginReceiveResult OnReceive( Packet* packet );
    virtual void OnClosedConnection( const SystemAddress& systemAddress, RakNetGUID rakNetGUID, PI2_LostConnectionReason lostConnectionReason );

    enum NATDetectionState
    {
        STATE_NONE,
        STATE_TESTING_NONE_1,
        STATE_TESTING_NONE_2,
        STATE_TESTING_FULL_CONE_1,
        STATE_TESTING_FULL_CONE_2,
        STATE_TESTING_ADDRESS_RESTRICTED_1,
        STATE_TESTING_ADDRESS_RESTRICTED_2,
        STATE_TESTING_PORT_RESTRICTED_1,
        STATE_TESTING_PORT_RESTRICTED_2,
        STATE_DONE,
    };

    struct NATDetectionAttempt
    {
        SystemAddress systemAddress;
        NATDetectionState detectionState;
        RakNet::TimeMS nextStateTime;
        RakNet::TimeMS timeBetweenAttempts;
        unsigned short c2Port;
        RakNetGUID guid;
    };

    virtual void OnRNS2Recv( RNS2RecvStruct* recvStruct );
    virtual void DeallocRNS2RecvStruct( RNS2RecvStruct* s, const char* file, unsigned int line );
    virtual RNS2RecvStruct* AllocRNS2RecvStruct( const char* file, unsigned int line );

protected:
    std::deque<RNS2RecvStruct*> bufferedPackets;
    std::mutex bufferedPacketsMutex;

    void OnDetectionRequest( Packet* packet );
    std::vector<NATDetectionAttempt> natDetectionAttempts;

    // s1p1 is rakpeer itself
    RakNetSocket2 *s1p2, *s2p3, *s3p4, *s4p5;
    std::string s3p4Address;
};

} // namespace RakNet

#endif // _RAKNET_SUPPORT_*
