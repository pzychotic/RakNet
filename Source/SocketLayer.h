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
/// \brief SocketLayer class implementation
///

#pragma once

#include "RakMemoryOverride.h"
#include "RakNetTypes.h"
#include "Export.h"
#include "MTUSize.h"
#include "RakString.h"

namespace RakNet {

// A platform independent implementation of Berkeley sockets, with settings used by RakNet
class RAK_DLL_EXPORT SocketLayer
{
public:
	
	/// Given a socket and IP, retrieves the subnet mask, on linux the socket is unused
	/// \param[in] inSock the socket 
	/// \param[in] inIpString The ip of the interface you wish to retrieve the subnet mask from
	/// \return Returns the ip dotted subnet mask if successful, otherwise returns empty string ("")
	static RakString GetSubNetForSocketAndIp(__UDPSOCKET__ inSock, RakString inIpString);

	/// Retrieve all local IP address in a string format.
	/// \param[in] s The socket whose port we are referring to
	/// \param[in] ipList An array of ip address in dotted notation.
	static void GetMyIP( SystemAddress addresses[MAXIMUM_NUMBER_OF_INTERNAL_IDS] );

	static unsigned short GetLocalPort( __UDPSOCKET__ s );
	static void GetSystemAddress_Old( __UDPSOCKET__ s, SystemAddress *systemAddressOut );
	static void GetSystemAddress( __UDPSOCKET__ s, SystemAddress *systemAddressOut );

	static void SetSocketOptions( __UDPSOCKET__ listenSocket, bool blockingSocket, bool setBroadcast );

	// AF_INET (default). For IPV6, use AF_INET6. To autoselect, use AF_UNSPEC.
	static bool GetFirstBindableIP(char firstBindable[128], int ipProto);
};

} // namespace RakNet
