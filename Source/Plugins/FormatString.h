/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant 
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

/// \file FormatString.h
///

#pragma once

#include "Export.h"

namespace RakNet {

char * FormatString(const char *format, ...);
char * FormatStringTS(char *output, const char *format, ...);

} // namespace RakNet
