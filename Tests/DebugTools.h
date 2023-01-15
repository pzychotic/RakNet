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

class DebugTools
{
public:
    DebugTools();
    ~DebugTools();
    static void ShowError( const std::string& errorString, bool pause, unsigned int lineNum, const char* fileName );
};
