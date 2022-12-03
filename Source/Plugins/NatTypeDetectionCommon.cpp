/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "Plugins/NatTypeDetectionCommon.h"

#if _RAKNET_SUPPORT_NatTypeDetectionServer == 1 || _RAKNET_SUPPORT_NatTypeDetectionClient == 1

#include "SocketIncludes.h"

namespace RakNet {

bool CanConnect( NATTypeDetectionResult type1, NATTypeDetectionResult type2 )
{
    /// If one system is NAT_TYPE_SYMMETRIC, the other must be NAT_TYPE_ADDRESS_RESTRICTED or less
    /// If one system is NAT_TYPE_PORT_RESTRICTED, the other must be NAT_TYPE_PORT_RESTRICTED or less
    bool connectionGraph[NAT_TYPE_COUNT][NAT_TYPE_COUNT] =
        {
            // None,	Full Cone,	Address Restricted,		Port Restricted,	Symmetric,	Unknown,	InProgress,	Supports_UPNP
            { true, true, true, true, true, false, false, true },       // None
            { true, true, true, true, true, false, false, true },       // Full Cone
            { true, true, true, true, true, false, false, true },       // Address restricted
            { true, true, true, true, false, false, false, true },      // Port restricted
            { true, true, true, false, false, false, false, true },     // Symmetric
            { false, false, false, false, false, false, false, false }, // Unknown
            { false, false, false, false, false, false, false, false }, // InProgress
            { true, true, true, true, true, false, false, true }        // Supports_UPNP
        };

    return connectionGraph[(int)type1][(int)type2];
}

const char* NATTypeDetectionResultToString( NATTypeDetectionResult type )
{
    switch( type )
    {
    case NAT_TYPE_NONE:
        return "None";
    case NAT_TYPE_FULL_CONE:
        return "Full cone";
    case NAT_TYPE_ADDRESS_RESTRICTED:
        return "Address restricted";
    case NAT_TYPE_PORT_RESTRICTED:
        return "Port restricted";
    case NAT_TYPE_SYMMETRIC:
        return "Symmetric";
    case NAT_TYPE_UNKNOWN:
        return "Unknown";
    case NAT_TYPE_DETECTION_IN_PROGRESS:
        return "In Progress";
    case NAT_TYPE_SUPPORTS_UPNP:
        return "Supports UPNP";
    case NAT_TYPE_COUNT:
        return "NAT_TYPE_COUNT";
    }
    return "Error, unknown enum in NATTypeDetectionResult";
}

// None and relaxed can connect to anything
// Moderate can connect to moderate or less
// Strict can connect to relaxed or less
const char* NATTypeDetectionResultToStringFriendly( NATTypeDetectionResult type )
{
    switch( type )
    {
    case NAT_TYPE_NONE:
        return "Open";
    case NAT_TYPE_FULL_CONE:
        return "Relaxed";
    case NAT_TYPE_ADDRESS_RESTRICTED:
        return "Relaxed";
    case NAT_TYPE_PORT_RESTRICTED:
        return "Moderate";
    case NAT_TYPE_SYMMETRIC:
        return "Strict";
    case NAT_TYPE_UNKNOWN:
        return "Unknown";
    case NAT_TYPE_DETECTION_IN_PROGRESS:
        return "In Progress";
    case NAT_TYPE_SUPPORTS_UPNP:
        return "Supports UPNP";
    case NAT_TYPE_COUNT:
        return "NAT_TYPE_COUNT";
    }
    return "Error, unknown enum in NATTypeDetectionResult";
}


RakNetSocket2* CreateNonblockingBoundSocket( const char* bindAddr
#ifdef __native_client__
                                             ,
                                             _PP_Instance_ chromeInstance
#endif
                                             ,
                                             RNS2EventHandler* eventHandler )
{
    RakNetSocket2* r2 = RakNetSocket2Allocator::AllocRNS2();
#if defined( __native_client__ )
    NativeClientBindParameters ncbp;
    RNS2_NativeClient* nativeClientSocket = (RNS2_NativeClient*)r2;
    ncbp.eventHandler = eventHandler;
    ncbp.forceHostAddress = (char*)bindAddr;
    ncbp.is_ipv6 = false;
    ncbp.nativeClientInstance = chromeInstance;
    ncbp.port = 0;
    nativeClientSocket->Bind( &ncbp, _FILE_AND_LINE_ );
#else
    if( r2->IsBerkleySocket() )
    {
        RNS2_BerkleyBindParameters bbp;
        bbp.port = 0;
        bbp.hostAddress = (char*)bindAddr;
        bbp.addressFamily = AF_INET;
        bbp.type = SOCK_DGRAM;
        bbp.protocol = 0;
        bbp.nonBlockingSocket = true;
        bbp.setBroadcast = true;
        bbp.setIPHdrIncl = false;
        bbp.doNotFragment = false;
        bbp.pollingThreadPriority = 0;
        bbp.eventHandler = eventHandler;
        RNS2BindResult br = ( (RNS2_Berkley*)r2 )->Bind( &bbp, _FILE_AND_LINE_ );

        if( br == BR_FAILED_TO_BIND_SOCKET )
        {
            RakNetSocket2Allocator::DeallocRNS2( r2 );
            return 0;
        }
        else if( br == BR_FAILED_SEND_TEST )
        {
            RakNetSocket2Allocator::DeallocRNS2( r2 );
            return 0;
        }
        else
        {
            RakAssert( br == BR_SUCCESS );
        }

        ( (RNS2_Berkley*)r2 )->CreateRecvPollingThread( 0 );
    }
    else
    {
        RakAssert( "TODO" && 0 );
    }
#endif

    return r2;
}

/*
int NatTypeRecvFrom(char *data, RakNetSocket2* socket, SystemAddress &sender, RNS2EventHandler *eventHandler)
{
#if defined(__native_client__)
    RakAssert("TODO" && 0);
#else
    if (socket->IsBerkleySocket())
    {
        RNS2RecvStruct *recvFromStruct;
        recvFromStruct=AllocRNS2RecvStruct(_FILE_AND_LINE_);
        if (recvFromStruct != NULL)
        {
            recvFromStruct->socket=this;
            socket->RecvFromBlocking(recvFromStruct);
        }
        if (recvFromStruct->bytesRead>0)
        {
            sender = recvFromStruct->systemAddress;
        }
        return recvFromStruct->bytesRead;
    }
    return 0;
#endif
}
*/

} // namespace RakNet

#endif // #if _RAKNET_SUPPORT_NatTypeDetectionServer==1 || _RAKNET_SUPPORT_NatTypeDetectionClient==1
