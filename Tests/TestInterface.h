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

#include <string>

class TestInterface
{
public:
    TestInterface();
    virtual ~TestInterface();
    virtual int RunTest( bool isVerbose, bool noPauses ) = 0; //should return 0 if no error, or the error number
    virtual std::string GetTestName() const = 0;
    virtual std::string ErrorCodeToString( int errorCode ) const = 0;
    virtual void DestroyPeers() = 0;
};
