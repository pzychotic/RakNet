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

#if defined( _WIN32 )
#include "WindowsIncludes.h"
#endif
#include <cstring>
#include <string>

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
    const size_t len = std::strlen( str );
    std::wstring s( len, L' ' );
    s.resize( std::mbstowcs( s.data(), str, len ) );
    s += L'\n';
#else
    std::string s( str );
    s += '\n';
#endif // UNICODE

    OutputDebugString( s.c_str() );

#endif // _WIN32
}

} // namespace RakNet

#endif // _RAKNET_SUPPORT_*
