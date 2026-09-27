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
/// \brief A RakNet plugin performing networking to communicate with UDPProxyCoordinator. Ultimately used to tell UDPProxyServer to forward UDP packets.

#pragma once

#include "NativeFeatureIncludes.h"
#if _RAKNET_SUPPORT_UDPProxyClient == 1

#include "Export.h"
#include "RakNetTypes.h"
#include "PluginInterface2.h"

#include <vector>

/// \defgroup UDP_PROXY_GROUP UDPProxy
/// \brief Forwards UDP datagrams from one system to another. Protocol independent
/// \details Used when NatPunchthroughClient fails
/// \ingroup PLUGINS_GROUP

namespace RakNet {

class UDPProxyClient;

/// Callback to handle results of calling UDPProxyClient::RequestForwarding()
/// \ingroup UDP_PROXY_GROUP
struct UDPProxyClientResultHandler
{
    UDPProxyClientResultHandler() {}
    virtual ~UDPProxyClientResultHandler() {}

    /// Called when our forwarding request was completed. We can now connect to \a targetAddress by using \a proxyAddress instead
    /// \param[out] proxyIPAddress IP Address of the proxy server, which will forward messages to targetAddress
    /// \param[out] proxyPort Remote port to use on the proxy server, which will forward messages to targetAddress
    /// \param[out] proxyCoordinator \a proxyCoordinator parameter originally passed to UDPProxyClient::RequestForwarding
    /// \param[out] sourceAddress Our address as the coordinator sees it. The \a sourceAddress passed to UDPProxyClient::RequestForwarding is ignored.
    /// \param[out] targetAddress \a targetAddress parameter originally passed to UDPProxyClient::RequestForwarding
    /// \param[out] targetGuid \a targetGuid parameter originally passed to UDPProxyClient::RequestForwarding
    /// \param[out] proxyClient The plugin that is calling this callback
    virtual void OnForwardingSuccess( const char* proxyIPAddress, unsigned short proxyPort,
                                      SystemAddress proxyCoordinator, SystemAddress sourceAddress, SystemAddress targetAddress, RakNetGUID targetGuid, UDPProxyClient* proxyClientPlugin ) = 0;

    /// Called when another system has setup forwarding, with our system as the target address.
    /// Plugin automatically sends a datagram to proxyIPAddress before this callback, to open our router if necessary.
    /// \param[out] proxyIPAddress IP Address of the proxy server, which will forward messages to targetAddress
    /// \param[out] proxyPort Remote port to use on the proxy server, which will forward messages to targetAddress
    /// \param[out] proxyCoordinator \a proxyCoordinator parameter originally passed to UDPProxyClient::RequestForwarding
    /// \param[out] sourceAddress Address of the remote system that requested forwarding and will be sending to us, as the coordinator sees it.
    /// \param[out] targetAddress \a targetAddress parameter originally passed to UDPProxyClient::RequestForwarding. This is our external IP address.
    /// \param[out] targetGuid \a targetGuid parameter originally passed to UDPProxyClient::RequestForwarding
    /// \param[out] proxyClient The plugin that is calling this callback
    virtual void OnForwardingNotification( const char* proxyIPAddress, unsigned short proxyPort,
                                           SystemAddress proxyCoordinator, SystemAddress sourceAddress, SystemAddress targetAddress, RakNetGUID targetGuid, UDPProxyClient* proxyClientPlugin ) = 0;

    /// Called when our forwarding request failed, because no UDPProxyServers are connected to UDPProxyCoordinator
    /// \param[out] proxyCoordinator \a proxyCoordinator parameter originally passed to UDPProxyClient::RequestForwarding
    /// \param[out] sourceAddress Our address as the coordinator sees it. The \a sourceAddress passed to UDPProxyClient::RequestForwarding is ignored.
    /// \param[out] targetAddress \a targetAddress parameter originally passed to UDPProxyClient::RequestForwarding
    /// \param[out] targetGuid \a targetGuid parameter originally passed to UDPProxyClient::RequestForwarding
    /// \param[out] proxyClient The plugin that is calling this callback
    virtual void OnNoServersOnline( SystemAddress proxyCoordinator, SystemAddress sourceAddress, SystemAddress targetAddress, RakNetGUID targetGuid, UDPProxyClient* proxyClientPlugin ) = 0;

    /// Called when our forwarding request failed, because no UDPProxyServers are connected to UDPProxyCoordinator
    /// \param[out] proxyCoordinator \a proxyCoordinator parameter originally passed to UDPProxyClient::RequestForwarding
    /// \param[out] sourceAddress Our address as the coordinator sees it. The \a sourceAddress passed to UDPProxyClient::RequestForwarding is ignored.
    /// \param[out] targetAddress \a targetAddress parameter originally passed to UDPProxyClient::RequestForwarding
    /// \param[out] targetGuid \a targetGuid parameter originally passed to UDPProxyClient::RequestForwarding
    /// \param[out] proxyClient The plugin that is calling this callback
    virtual void OnRecipientNotConnected( SystemAddress proxyCoordinator, SystemAddress sourceAddress, SystemAddress targetAddress, RakNetGUID targetGuid, UDPProxyClient* proxyClientPlugin ) = 0;

    /// Called when our forwarding request failed, because all UDPProxyServers that are connected to UDPProxyCoordinator are at their capacity
    /// Either add more servers, or increase capacity via UDPForwarder::SetMaxForwardEntries()
    /// \param[out] proxyCoordinator \a proxyCoordinator parameter originally passed to UDPProxyClient::RequestForwarding
    /// \param[out] sourceAddress Our address as the coordinator sees it. The \a sourceAddress passed to UDPProxyClient::RequestForwarding is ignored.
    /// \param[out] targetAddress \a targetAddress parameter originally passed to UDPProxyClient::RequestForwarding
    /// \param[out] targetGuid \a targetGuid parameter originally passed to UDPProxyClient::RequestForwarding
    /// \param[out] proxyClient The plugin that is calling this callback
    virtual void OnAllServersBusy( SystemAddress proxyCoordinator, SystemAddress sourceAddress, SystemAddress targetAddress, RakNetGUID targetGuid, UDPProxyClient* proxyClientPlugin ) = 0;

    /// Called when our forwarding request is already in progress on the \a proxyCoordinator.
    /// This can be ignored, but indicates an unneeded second request
    /// \param[out] proxyIPAddress IP Address of the proxy server, which is forwarding messages to targetAddress
    /// \param[out] proxyPort Remote port to use on the proxy server, which is forwarding messages to targetAddress
    /// \param[out] proxyCoordinator \a proxyCoordinator parameter originally passed to UDPProxyClient::RequestForwarding
    /// \param[out] sourceAddress Our address as the coordinator sees it. The \a sourceAddress passed to UDPProxyClient::RequestForwarding is ignored.
    /// \param[out] targetAddress \a targetAddress parameter originally passed to UDPProxyClient::RequestForwarding
    /// \param[out] targetGuid \a targetGuid parameter originally passed to UDPProxyClient::RequestForwarding
    /// \param[out] proxyClient The plugin that is calling this callback
    virtual void OnForwardingInProgress( const char* proxyIPAddress, unsigned short proxyPort, SystemAddress proxyCoordinator, SystemAddress sourceAddress, SystemAddress targetAddress, RakNetGUID targetGuid, UDPProxyClient* proxyClientPlugin ) = 0;
};


/// \brief Communicates with UDPProxyCoordinator, in order to find a UDPProxyServer to forward our datagrams.
/// \details When NAT Punchthrough fails, it is possible to use a non-NAT system to forward messages from us to the recipient, and vice-versa.<BR>
/// The class to forward messages is UDPForwarder, and it is triggered over the network via the UDPProxyServer plugin.<BR>
/// The UDPProxyClient connects to UDPProxyCoordinator to get a list of servers running UDPProxyServer, and the coordinator will relay our forwarding request
///
/// A Peer acts on a coordinator's message only if it asked for it or the coordinator is one it designated.
/// A result of RequestForwarding() is taken only from the coordinator it went to, while that request is outstanding.
/// A request to ping the proxy servers, and a notification that another System set up forwarding to this Peer, are taken
/// only from a coordinator designated with AddCoordinator(), since a forwarding target has asked for nothing.
/// Anything else is consumed and dropped.
///
/// Residual risk: a proxy server's pong is an offline datagram, matched to its ping only by the server's address.
/// A System that can spoof that address can report a low ping and so steer which proxy server is chosen.
/// Closing that needs a nonce in the ping, which would change the core ping layout.
/// \sa NatPunchthroughServer
/// \sa NatPunchthroughClient
/// \ingroup UDP_PROXY_GROUP
class RAK_DLL_EXPORT UDPProxyClient : public PluginInterface2
{
public:
    // GetInstance() and DestroyInstance(instance*)
    STATIC_FACTORY_DECLARATIONS( UDPProxyClient )

    /// How many RequestForwarding() calls may await their final result at once. At the cap, RequestForwarding() returns false.
    static constexpr unsigned int MAX_OUTSTANDING_REQUESTS = 64;

    UDPProxyClient();
    ~UDPProxyClient();

    /// Receives the results of calling RequestForwarding()
    /// Set before calling RequestForwarding or you won't know what happened
    /// \param[in] resultHandler
    void SetResultHandler( UDPProxyClientResultHandler* rh );

    /// \brief Designates the System connected at \a systemAddress as a UDPProxyCoordinator this Peer takes unrequested messages from.
    /// \details Call it on every Peer that may be a forwarding target. Only a Designated coordinator can make this Peer ping a
    /// proxy server, which is how a target behind NAT opens its router to the proxy, and only its notifications reach
    /// UDPProxyClientResultHandler::OnForwardingNotification(). The coordinator also asks the requester to ping the proxy servers;
    /// an undesignated requester ignores that, and the coordinator goes on without its pings once its ping timeout passes.
    ///
    /// The designation is by address. It may be made before the coordinator connects, and lapses when the connection at that
    /// address closes: a System that connects from the same address after that is not Designated until AddCoordinator() is
    /// called again. Shutting the Peer down drops every designation.
    /// \param[in] systemAddress The coordinator's address, as this Peer sees its connection.
    void AddCoordinator( const SystemAddress& systemAddress );

    /// Withdraws a designation made with AddCoordinator().
    void RemoveCoordinator( const SystemAddress& systemAddress );

    /// Sends a request to proxyCoordinator to find a server and have that server setup UDPForwarder::StartForwarding() on our address to \a targetAddressAsSeenFromCoordinator
    /// The forwarded datagrams can be from any UDP source, not just RakNet
    /// \pre Must be connected to \a proxyCoordinator
    /// \pre Systems running UDPProxyServer must be connected to \a proxyCoordinator and logged in via UDPProxyCoordinator::LoginServer() or UDPProxyServer::LoginToCoordinator()
    /// \note May still fail, if all proxy servers have no open connections.
    /// \note RakNet's protocol will ensure a message is sent at least every 5 seconds, so if routing RakNet messages, it is a reasonable value for timeoutOnNoDataMS, plus an extra few seconds for latency.
    /// \param[in] proxyCoordinator System we are connected to that is running the UDPProxyCoordinator plugin
    /// \param[in] sourceAddress Ignored, and kept for the frozen message layout: the coordinator forwards only from the requester, at its address as
    /// the coordinator sees it. Pass UNASSIGNED_SYSTEM_ADDRESS. To forward from another system, that system has to request it itself.
    /// \param[in] targetAddressAsSeenFromCoordinator External IP address of the system we want to forward messages to. If this system is connected to UDPProxyCoordinator at this address using RakNet, that system will ping the server and thus open the router for incoming communication. In any other case, you are responsible for doing your own network communication to have that system ping the server. See also targetGuid in the other version of RequestForwarding(), to avoid the need to know the IP address to the coordinator of the destination.
    /// \param[in] timeoutOnNoData If no data is sent by the forwarded systems, how long before removing the forward entry from UDPForwarder? UDP_FORWARDER_MAXIMUM_TIMEOUT is the maximum value. Recommended 10 seconds.
    /// \param[in] serverSelectionBitstream If you want to send data to UDPProxyCoordinator::GetBestServer(), write it here
    /// \note The request stays outstanding until its final result arrives, the connection to \a proxyCoordinator closes, or \a timeoutOnNoDataMS passes.
    /// Only a result that matches an outstanding request reaches the result handler. At most MAX_OUTSTANDING_REQUESTS may be outstanding;
    /// asking again for a target already outstanding with \a proxyCoordinator restarts its timeout and takes no new entry.
    /// A result that arrives after \a timeoutOnNoDataMS is dropped without a callback. With more than one proxy server the coordinator
    /// may wait about three seconds for pings before it answers, so pass comfortably more than that.
    /// \return true if the request was sent, false if we are not connected to proxyCoordinator, no result handler is set, or MAX_OUTSTANDING_REQUESTS are outstanding
    bool RequestForwarding( SystemAddress proxyCoordinator, SystemAddress sourceAddress, SystemAddress targetAddressAsSeenFromCoordinator, RakNet::TimeMS timeoutOnNoDataMS, BitStream* serverSelectionBitstream = 0 );

    /// Same as above, but specify the target with a GUID, in case you don't know what its address is to the coordinator
    /// If requesting forwarding to a RakNet enabled system, then it is easier to use targetGuid instead of targetAddressAsSeenFromCoordinator
    bool RequestForwarding( SystemAddress proxyCoordinator, SystemAddress sourceAddress, RakNetGUID targetGuid, RakNet::TimeMS timeoutOnNoDataMS, BitStream* serverSelectionBitstream = 0 );

    /// \internal
    virtual void Update( void );
    virtual PluginReceiveResult OnReceive( Packet* packet );
    virtual void OnRakPeerShutdown( void );
    virtual void OnClosedConnection( const SystemAddress& systemAddress, RakNetGUID rakNetGUID, PI2_LostConnectionReason lostConnectionReason );

    struct ServerWithPing
    {
        unsigned short ping;
        SystemAddress serverAddress;
    };
    struct SenderAndTargetAddress
    {
        SystemAddress senderClientAddress;
        SystemAddress targetClientAddress;
    };
    struct PingServerGroup
    {
        SenderAndTargetAddress sata;
        RakNet::TimeMS startPingTime;
        SystemAddress coordinatorAddressForPings;
        std::vector<ServerWithPing> serversToPing;
        bool AreAllServersPinged( void ) const;
        void SendPingedServersToCoordinator( RakPeerInterface* rakPeerInterface );
    };
    std::vector<PingServerGroup*> pingServerGroups;

protected:
    // A RequestForwarding() call awaiting its final result. It has no source: the coordinator ignores the one passed and forwards from this Peer.
    struct OutstandingRequest
    {
        SystemAddress coordinatorAddress;
        bool usesAddress;
        SystemAddress targetAddress;
        RakNetGUID targetGuid;
        RakNet::TimeMS requestTime;
        RakNet::TimeMS timeoutOnNoDataMS;
    };

    void OnPingServers( Packet* packet );
    bool IsDesignatedCoordinator( const SystemAddress& systemAddress ) const;
    bool AddOutstandingRequest( const OutstandingRequest& request );
    std::vector<OutstandingRequest>::iterator FindOutstandingRequest( const SystemAddress& coordinatorAddress, const SystemAddress& targetAddress, RakNetGUID targetGuid );
    void Clear( void );
    UDPProxyClientResultHandler* resultHandler;
    std::vector<SystemAddress> coordinators;
    std::vector<OutstandingRequest> outstandingRequests;
};

} // namespace RakNet

#endif // _RAKNET_SUPPORT_*
