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

#include "RakPeerInterface.h"
#include "MessageIdentifiers.h"
#include "BitStream.h"
#include "RakPeer.h"
#include "CommonFunctions.h"
#include "RakTimer.h"

using namespace RakNet;

class TestHelpers
{
public:
    TestHelpers( void );
    ~TestHelpers( void );

    static bool WaitAndConnectTwoPeersLocally( RakPeerInterface* connector, RakPeerInterface* connectee, int millisecondsToWait );
    static bool ConnectTwoPeersLocally( RakPeerInterface* connector, RakPeerInterface* connectee );
    static bool BroadCastTestPacket( RakPeerInterface* sender, PacketReliability rel = RELIABLE_ORDERED, PacketPriority pr = HIGH_PRIORITY, int typeNum = ID_USER_PACKET_ENUM + 1 );
    static bool WaitForTestPacket( RakPeerInterface* reciever, int millisecondsToWait );
    static bool SendTestPacketDirected( RakPeerInterface* sender, char* ip, int port, PacketReliability rel = RELIABLE_ORDERED, PacketPriority pr = HIGH_PRIORITY, int typeNum = ID_USER_PACKET_ENUM + 1 );
};
