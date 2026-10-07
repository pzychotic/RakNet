#pragma once

#include "Plugins/UDPProxyCommon.h"
#include "Plugins/UDPProxyCoordinator.h"

#include "BitStream.h"
#include "MessageIdentifiers.h"
#include "RakNetTime.h"
#include "RakNetTypes.h"

#include <vector>

/*
The UDPProxy Messages a test writes in place of UDPProxyClient, each laid out field for
field as UDPProxyClient writes it, and in that order so a wire-format change shows up here
as a diff.
*/

namespace UDPProxyWire {

// The forwarding timeout every request here carries. The coordinator only passes it on.
constexpr RakNet::TimeMS kForwardingTimeoutMs = 10000;

/// One entry of a ping reply: the ping a client measured to one proxy server.
inline RakNet::UDPProxyCoordinator::ServerWithPing ServerPing( const RakNet::SystemAddress& serverAddress, unsigned short ping )
{
    RakNet::UDPProxyCoordinator::ServerWithPing swp;
    swp.serverAddress = serverAddress;
    swp.ping = ping;
    return swp;
}

/// ID_UDP_PROXY_FORWARDING_REQUEST_FROM_CLIENT_TO_COORDINATOR naming the target by address,
/// with selectionBytes bytes of server selection data if not 0.
inline void WriteForwardingRequest( RakNet::BitStream& bs, const RakNet::SystemAddress& source, const RakNet::SystemAddress& target,
                                    unsigned int selectionBytes = 0 )
{
    bs.Write( (RakNet::MessageID)RakNet::ID_UDP_PROXY_GENERAL );
    bs.Write( (RakNet::MessageID)RakNet::ID_UDP_PROXY_FORWARDING_REQUEST_FROM_CLIENT_TO_COORDINATOR );
    bs.Write( source );
    bs.Write( true ); // target by address
    bs.Write( target );
    bs.Write( kForwardingTimeoutMs );
    bs.Write( selectionBytes > 0 );
    if( selectionBytes > 0 )
    {
        RakNet::BitStream selection;
        for( unsigned int i = 0; i < selectionBytes; i++ )
            selection.Write( (unsigned char)i );
        bs.Write( &selection );
    }
}

/// ID_UDP_PROXY_FORWARDING_REQUEST_FROM_CLIENT_TO_COORDINATOR naming the target by GUID, with
/// no server selection data.
inline void WriteForwardingRequest( RakNet::BitStream& bs, const RakNet::SystemAddress& source, RakNet::RakNetGUID target )
{
    bs.Write( (RakNet::MessageID)RakNet::ID_UDP_PROXY_GENERAL );
    bs.Write( (RakNet::MessageID)RakNet::ID_UDP_PROXY_FORWARDING_REQUEST_FROM_CLIENT_TO_COORDINATOR );
    bs.Write( source );
    bs.Write( false ); // target by GUID
    bs.Write( target );
    bs.Write( kForwardingTimeoutMs );
    bs.Write( false );
}

/// ID_UDP_PROXY_PING_SERVERS_REPLY_FROM_CLIENT_TO_COORDINATOR for the (source, target) pair.
inline void WritePingReply( RakNet::BitStream& bs, const RakNet::SystemAddress& source, const RakNet::SystemAddress& target,
                            const std::vector<RakNet::UDPProxyCoordinator::ServerWithPing>& pings )
{
    bs.Write( (RakNet::MessageID)RakNet::ID_UDP_PROXY_GENERAL );
    bs.Write( (RakNet::MessageID)RakNet::ID_UDP_PROXY_PING_SERVERS_REPLY_FROM_CLIENT_TO_COORDINATOR );
    bs.Write( source );
    bs.Write( target );
    bs.Write( (unsigned short)pings.size() );
    for( const RakNet::UDPProxyCoordinator::ServerWithPing& swp : pings )
    {
        bs.Write( swp.serverAddress );
        bs.Write( swp.ping );
    }
}

} // namespace UDPProxyWire
