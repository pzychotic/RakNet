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
#include "GetTime.h"
#include "MessageIdentifiers.h"
#include "BitStream.h"
#include <cstdio>
#include <memory.h>
#include <cstring>
#include <stdlib.h>
#include "Rand.h"
#include "RakNetStatistics.h"
#include "RakMemoryOverride.h"
#include "DebugTools.h"

#include <vector>

using namespace RakNet;
class ReliableOrderedConvertedTest : public TestInterface
{
public:
    ReliableOrderedConvertedTest( void );
    ~ReliableOrderedConvertedTest( void );
    int RunTest( bool isVerbose, bool noPauses ); //should return 0 if no error, or the error number
    std::string GetTestName() const;
    std::string ErrorCodeToString( int errorCode ) const;
    void DestroyPeers();

protected:
    void* LoggedMalloc( size_t size, const char* file, unsigned int line );
    void LoggedFree( void* p, const char* file, unsigned int line );
    void* LoggedRealloc( void* p, size_t size, const char* file, unsigned int line );

private:
    std::vector<RakPeerInterface*> destroyList;
};
