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

#include <stdint.h>

namespace RakNet {

// From http://www.azillionmonkeys.com/qed/hash.html
// Author of main code is Paul Hsieh
// Also note http://burtleburtle.net/bob/hash/doobs.html, which shows that this is 20% faster than the one on that page but has more collisions

uint32_t SuperFastHashIncremental( const char* data, int len, unsigned int lastHash );

} // namespace RakNet
