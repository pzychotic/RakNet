/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "TestHelpers.h"

#include "ConnectionWaits.h"

TestHelpers::TestHelpers( void )
{
}

TestHelpers::~TestHelpers( void )
{
}

//returns false if not connected
bool TestHelpers::WaitAndConnectTwoPeersLocally( RakPeerInterface* connector, RakPeerInterface* connectee, int millisecondsToWait )
{

    SystemAddress connecteeAdd = connectee->GetInternalID();
    return CommonFunctions::WaitAndConnect( connector, "127.0.0.1", connecteeAdd.GetPort(), millisecondsToWait );
}

bool TestHelpers::BroadCastTestPacket( RakPeerInterface* sender, PacketReliability rel, PacketPriority pr, int typeNum ) //returns send return value
{

    char str2[] = "AAAAAAAAAA";
    str2[0] = typeNum;
    return sender->Send( str2, (int)strlen( str2 ) + 1, pr, rel, 0, UNASSIGNED_SYSTEM_ADDRESS, true ) > 0;
}

bool TestHelpers::WaitForTestPacket( RakPeerInterface* reciever, int millisecondsToWait )
{
    return ConnectionWaits::WaitForMessage( reciever, ID_USER_PACKET_ENUM + 1, millisecondsToWait );
}
