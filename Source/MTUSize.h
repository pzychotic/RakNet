/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

/// \file
/// \brief \b [Internal] Defines the default maximum transfer unit.
///

#pragma once

#ifndef MAXIMUM_MTU_SIZE

/// \li \em 17914 16 Mbit/Sec Token Ring
/// \li \em 4464 4 Mbits/Sec Token Ring
/// \li \em 4352 FDDI
/// \li \em 1500. The largest Ethernet packet size \b recommended. This is the typical setting for non-PPPoE, non-VPN connections. The default value for NETGEAR routers, adapters and switches.
/// \li \em 1492. The size PPPoE prefers.
/// \li \em 1472. Maximum size to use for pinging. (Bigger packets are fragmented.)
/// \li \em 1468. The size DHCP prefers.
/// \li \em 1460. Usable by AOL if you don't have large email attachments, etc.
/// \li \em 1430. The size VPN and PPTP prefer.
/// \li \em 1400. Maximum size for AOL DSL.
/// \li \em 576. Typical value to connect to dial-up ISPs.
/// The largest value for an UDP datagram

#define MAXIMUM_MTU_SIZE 1492
#define MINIMUM_MTU_SIZE 400

#endif

#include "NativeFeatureIncludes.h"
#include "SecureHandshake.h"

/// The lowest MTU a connection can end up using. RakPeer offers { MAXIMUM_MTU_SIZE, 1200,
/// 576 } in descending order during the handshake and there is no API to set an MTU
/// directly, so no connection settles below this. Note this is *not* MINIMUM_MTU_SIZE,
/// which is a floor on what the code tolerates rather than a value it ever selects.
#define MINIMUM_NEGOTIATED_MTU_SIZE 576

#if LIBCAT_SECURITY == 1
/// Encryption overhead ReliabilityLayer::Reset subtracts from the MTU before the congestion
/// manager ever sees it, so it comes off the usable payload of every datagram.
#define RAKNET_DATAGRAM_SECURITY_OVERHEAD_BYTES ( cat::AuthenticatedEncryption::OVERHEAD_BYTES )
#else
#define RAKNET_DATAGRAM_SECURITY_OVERHEAD_BYTES 0
#endif

namespace RakNet {

/// The largest message RakPeer::Send, Send( BitStream* ) and SendList accept, in bytes.
///
/// Derived, not chosen. It is MAXIMUM_SPLIT_PACKET_COUNT - the receive-side cap in
/// ReliabilityLayer.h, 65536 - multiplied by the payload one datagram carries at the
/// smallest MTU a connection can negotiate:
///
///     MINIMUM_NEGOTIATED_MTU_SIZE
///       - UDP_HEADER_SIZE                                    (28, CCRakNetSlidingWindow.h)
///       - DatagramHeaderFormat::GetDataHeaderByteLength()     (9, ReliabilityLayer.cpp)
///       - BITS_TO_BYTES( GetMaxMessageHeaderLengthBits() )   (23, ReliabilityLayer.cpp)
///       - RAKNET_DATAGRAM_SECURITY_OVERHEAD_BYTES
///
/// = 516 bytes in a default build, so 65536 x 516 = 33,816,576.
///
/// Deriving at the *floor* rather than at the MTU actually negotiated is what makes the two
/// limits agree by construction: a message this Peer is willing to emit is one every Peer
/// will accept, whichever MTU the two ends land on and whichever target a broadcast reaches.
/// A connection at MTU 1492 could carry nearly three times as much, but sizing to that would
/// mean Send succeeding or failing on the outcome of a handshake the caller never saw.
///
/// The four numbers above cannot be named here: DatagramHeaderFormat is local to
/// ReliabilityLayer.cpp and the MTU list is local to RakPeer.cpp. A static_assert in each of
/// those two translation units checks this arithmetic against the real definitions, so
/// changing a header layout or the MTU list breaks the build instead of silently letting the
/// send and receive limits drift apart.
constexpr unsigned int MAXIMUM_MESSAGE_SIZE =
    65536u * ( MINIMUM_NEGOTIATED_MTU_SIZE - 28u - 9u - 23u - RAKNET_DATAGRAM_SECURITY_OVERHEAD_BYTES );

} // namespace RakNet
