/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "NativeFeatureIncludes.h"
#if _RAKNET_SUPPORT_PacketLogger == 1

#include "Plugins/PacketOutputWindowLogger.h"

#if defined( UNICODE )
#include "RakWString.h"
#endif
#include "RakString.h"
#if defined( _WIN32 )
#include "WindowsIncludes.h"
#endif

namespace RakNet {

PacketOutputWindowLogger::PacketOutputWindowLogger()
{
}
PacketOutputWindowLogger::~PacketOutputWindowLogger()
{
}
void PacketOutputWindowLogger::WriteLog( const char* str )
{
#if defined( _WIN32 )
#if defined( UNICODE )
    RakWString str2 = str;
    str2 += "\n";
    OutputDebugString( str2.C_String() );
#else
    RakString str2 = str;
    str2 += "\n";
    OutputDebugString( str2.C_String() );
#endif // UNICODE
#endif // _WIN32
}

} // namespace RakNet

#endif // _RAKNET_SUPPORT_*
