/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant 
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

/// \file FileOperations.h
///

#pragma once

#include "NativeFeatureIncludes.h"
#if _RAKNET_SUPPORT_FileOperations==1

#include "Export.h"

namespace RakNet {

bool RAK_DLL_EXPORT WriteFileWithDirectories( const char *path, char *data, unsigned dataLength );
bool RAK_DLL_EXPORT IsSlash(unsigned char c);
void RAK_DLL_EXPORT AddSlash( char *input );
void RAK_DLL_EXPORT QuoteIfSpaces(char *str);
bool RAK_DLL_EXPORT DirectoryExists(const char *directory);
unsigned int RAK_DLL_EXPORT GetFileLength(const char *path);

} // namespace RakNet

#endif // _RAKNET_SUPPORT_FileOperations
