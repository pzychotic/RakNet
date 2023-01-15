/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include <cassert>
#include <iostream>
#include <limits>

#include "DebugTools.h"

DebugTools::DebugTools()
{
}

DebugTools::~DebugTools()
{
}

void DebugTools::ShowError( const std::string& errorString, bool pause, unsigned int lineNum, const char* fileName )
{
    printf( "%s\nFile:%s \nLine: %i\n", errorString.c_str(), fileName, lineNum );
    assert( false );

    //if (pause)
    //{
    //  printf("Press enter to continue \n");
    //  std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    //  char pauseChar = std::cin.get();
    //}
}
