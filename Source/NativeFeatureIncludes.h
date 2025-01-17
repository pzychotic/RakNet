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

// If you want to change these defines, put them in NativeFeatureIncludesOverrides so your changes are not lost when updating RakNet
// The user should not edit this file
#include "NativeFeatureIncludesOverrides.h"

// Uncomment below defines, and paste to NativeFeatureIncludesOverrides.h, to exclude plugins that you do not want to build into the static library, or DLL
// These are not all the plugins, only those that are in the core library
// Other plugins are located in DependentExtensions
// #define _RAKNET_SUPPORT_MessageFilter 0
// #define _RAKNET_SUPPORT_NatPunchthroughClient 0
// #define _RAKNET_SUPPORT_NatPunchthroughServer 0
// #define _RAKNET_SUPPORT_NatTypeDetectionClient 0
// #define _RAKNET_SUPPORT_NatTypeDetectionServer 0
// #define _RAKNET_SUPPORT_PacketLogger 0
// #define _RAKNET_SUPPORT_Router2 0
// #define _RAKNET_SUPPORT_RPC4Plugin 0
// #define _RAKNET_SUPPORT_UDPProxyClient 0
// #define _RAKNET_SUPPORT_UDPProxyCoordinator 0
// #define _RAKNET_SUPPORT_UDPProxyServer 0
// #define _RAKNET_SUPPORT_ConsoleServer 0
// #define _RAKNET_SUPPORT_TelnetTransport 0
// #define _RAKNET_SUPPORT_TCPInterface 0
// #define _RAKNET_SUPPORT_LogCommandParser 0
// #define _RAKNET_SUPPORT_RakNetCommandParser 0
// #define _RAKNET_SUPPORT_EmailSender 0
// #define _RAKNET_SUPPORT_PacketizedTCP 0
// #define _RAKNET_SUPPORT_TwoWayAuthentication 0

// SET DEFAULTS IF UNDEFINED
#ifndef LIBCAT_SECURITY
#define LIBCAT_SECURITY 0
#endif
#ifndef _RAKNET_SUPPORT_MessageFilter
#define _RAKNET_SUPPORT_MessageFilter 1
#endif
#ifndef _RAKNET_SUPPORT_NatPunchthroughClient
#define _RAKNET_SUPPORT_NatPunchthroughClient 1
#endif
#ifndef _RAKNET_SUPPORT_NatPunchthroughServer
#define _RAKNET_SUPPORT_NatPunchthroughServer 1
#endif
#ifndef _RAKNET_SUPPORT_NatTypeDetectionClient
#define _RAKNET_SUPPORT_NatTypeDetectionClient 1
#endif
#ifndef _RAKNET_SUPPORT_NatTypeDetectionServer
#define _RAKNET_SUPPORT_NatTypeDetectionServer 1
#endif
#ifndef _RAKNET_SUPPORT_PacketLogger
#define _RAKNET_SUPPORT_PacketLogger 1
#endif
#ifndef _RAKNET_SUPPORT_Router2
#define _RAKNET_SUPPORT_Router2 1
#endif
#ifndef _RAKNET_SUPPORT_RPC4Plugin
#define _RAKNET_SUPPORT_RPC4Plugin 1
#endif
#ifndef _RAKNET_SUPPORT_UDPProxyClient
#define _RAKNET_SUPPORT_UDPProxyClient 1
#endif
#ifndef _RAKNET_SUPPORT_UDPProxyCoordinator
#define _RAKNET_SUPPORT_UDPProxyCoordinator 1
#endif
#ifndef _RAKNET_SUPPORT_UDPProxyServer
#define _RAKNET_SUPPORT_UDPProxyServer 1
#endif
#ifndef _RAKNET_SUPPORT_ConsoleServer
#define _RAKNET_SUPPORT_ConsoleServer 1
#endif
#ifndef _RAKNET_SUPPORT_TelnetTransport
#define _RAKNET_SUPPORT_TelnetTransport 1
#endif
#ifndef _RAKNET_SUPPORT_TCPInterface
#define _RAKNET_SUPPORT_TCPInterface 1
#endif
#ifndef _RAKNET_SUPPORT_LogCommandParser
#define _RAKNET_SUPPORT_LogCommandParser 1
#endif
#ifndef _RAKNET_SUPPORT_RakNetCommandParser
#define _RAKNET_SUPPORT_RakNetCommandParser 1
#endif
#ifndef _RAKNET_SUPPORT_PacketizedTCP
#define _RAKNET_SUPPORT_PacketizedTCP 1
#endif
#ifndef _RAKNET_SUPPORT_TwoWayAuthentication
#define _RAKNET_SUPPORT_TwoWayAuthentication 1
#endif
#ifndef _RAKNET_SUPPORT_UDPForwarder
#define _RAKNET_SUPPORT_UDPForwarder 1
#endif
#ifndef _RAKNET_SUPPORT_StatisticsHistory
#define _RAKNET_SUPPORT_StatisticsHistory 1
#endif
#ifndef _RAKNET_SUPPORT_RelayPlugin
#define _RAKNET_SUPPORT_RelayPlugin 1
#endif

// Take care of dependencies
#if _RAKNET_SUPPORT_TelnetTransport == 1
#undef _RAKNET_SUPPORT_PacketizedTCP
#define _RAKNET_SUPPORT_PacketizedTCP 1
#endif
#if _RAKNET_SUPPORT_PacketizedTCP == 1
#undef _RAKNET_SUPPORT_TCPInterface
#define _RAKNET_SUPPORT_TCPInterface 1
#endif
