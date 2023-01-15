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

#include "TestInterface.h"

#include "RakPeerInterface.h"
#include "MessageIdentifiers.h"
#include "BitStream.h"
#include "RakPeer.h"
#include "RakNetTime.h"
#include "GetTime.h"
#include "DebugTools.h"
#include "Rand.h" // randomMT
#include "RakTimer.h"
#include <cstdio>
#include "CommonFunctions.h"

using namespace RakNet;
class DroppedConnectionConvertTest : public TestInterface
{
public:
    DroppedConnectionConvertTest( void );
    ~DroppedConnectionConvertTest( void );
    int RunTest( bool isVerbose, bool noPauses ); //should return 0 if no error, or the error number
    std::string GetTestName() const;
    std::string ErrorCodeToString( int errorCode ) const;
    void DestroyPeers();

private:
    DataStructures::List<RakPeerInterface*> destroyList;
};
