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
/// \brief Router2 plugin. Allows you to connect to a system by routing packets through another system that is connected to both you and the destination. Useful for getting around NATs.
///

#pragma once

#include "NativeFeatureIncludes.h"
#if _RAKNET_SUPPORT_Router2 == 1 && _RAKNET_SUPPORT_UDPForwarder == 1

#include "RakNetTypes.h"
#include "PluginInterface2.h"
#include "Export.h"
#include "UDPForwarder.h"
#include "MessageIdentifiers.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace RakNet {

struct Router2DebugInterface
{
    Router2DebugInterface() {}
    virtual ~Router2DebugInterface() {}
    virtual void ShowFailure( const char* message );
    virtual void ShowDiagnostic( const char* message );
};

/// \defgroup ROUTER_2_GROUP Router2
/// \brief Part of the NAT punchthrough solution, allowing you to connect to systems by routing through a shared connection.
/// \details Router2 routes datagrams between two systems that are not directly connected by using the bandwidth of a third system, to which the other two systems were connected
/// It is of benefit when a fully connected mesh topology is desired, but could not be completely established due to routers and/or firewalls
/// As the system address of a remote system will be the system address of the intermediary, it is necessary to use the RakNetGUID object to refer to systems, including with other plugins
/// \ingroup PLUGINS_GROUP

/// \ingroup ROUTER_2_GROUP
/// \brief Class interface for the Router2 system
/// \details A Peer acts on a claim that forwarding is set up only if it asked for it or the
/// claim comes from a System it designated. As the source, it takes ID_ROUTER_2_FORWARDING_ESTABLISHED
/// only from the router it asked, while its request is outstanding. As the endpoint, it takes
/// ID_ROUTER_2_REROUTED only from a router designated with AddIntermediary(), and moves a live
/// connection only if that connection is already forwarded. A direct connection is never
/// rerouted. Anything else is consumed and dropped.
class RAK_DLL_EXPORT Router2 : public PluginInterface2
{
public:
    // GetInstance() and DestroyInstance(instance*)
    STATIC_FACTORY_DECLARATIONS( Router2 )

    Router2();
    virtual ~Router2();

    /// Sets the socket family to use, either IPV4 or IPV6
    /// \param[in] socketFamily For IPV4, use AF_INET (default). For IPV6, use AF_INET6. To autoselect, use AF_UNSPEC.
    void SetSocketFamily( unsigned short _socketFamily );

    /// \brief Query all connected systems to connect through them to a third system.
    /// System will return ID_ROUTER_2_FORWARDING_NO_PATH if unable to connect.
    /// Else you will get ID_ROUTER_2_FORWARDING_ESTABLISHED
    ///
    /// On ID_ROUTER_2_FORWARDING_ESTABLISHED, EstablishRouting as follows:
    ///
    /// BitStream bs(packet->data, packet->length, false);
    /// bs.IgnoreBytes(sizeof(MessageID));
    /// RakNetGUID endpointGuid;
    /// bs.Read(endpointGuid);
    /// unsigned short sourceToDestPort;
    /// bs.Read(sourceToDestPort);
    /// char ipAddressString[32];
    /// packet->systemAddress.ToString(false, ipAddressString);
    /// rakPeerInterface->EstablishRouting(ipAddressString, sourceToDestPort, 0,0);
    ///
    /// \note The SystemAddress for a connection should not be used - always use RakNetGuid as the address can change at any time.
    /// When the address changes, you will get ID_ROUTER_2_REROUTED
    void EstablishRouting( RakNetGUID endpointGuid );

    /// Set the maximum number of bidirectional connections this system will support
    /// Defaults to 0
    void SetMaximumForwardingRequests( int max );

    /// \brief Designates the System connected at \a systemAddress as a router this Peer, as an endpoint, accepts ID_ROUTER_2_REROUTED from.
    /// \details A new forwarded connection needs no designation. It matters when a connection
    /// forwarded to this Peer loses its router, and the source re-routes it through another:
    /// only a Designated router can move that connection to its new address. Designate every
    /// System that may route to this Peer, including the first router, since the move checks the
    /// connection against what that router announced.
    ///
    /// The designation is by address. It may be made before the router connects, and lapses when
    /// the connection at that address closes: a System that connects from the same address after
    /// that is not Designated until AddIntermediary() is called again.
    /// \param[in] systemAddress The router's address, as this Peer sees its connection.
    void AddIntermediary( const SystemAddress& systemAddress );

    /// Withdraws a designation made with AddIntermediary().
    void RemoveIntermediary( const SystemAddress& systemAddress );

    /// \brief Caps how many forwarded connections one Designated router may announce to this Peer ahead of their connection.
    /// \details Each ID_ROUTER_2_REROUTED for an endpoint not yet connected records an entry,
    /// freed when that endpoint's connection closes or the router's does. At the cap a further
    /// announcement is dropped and counted in GetPendingForwardsRefused().
    /// Defaults to 16.
    void SetMaxPendingForwardsPerIntermediary( unsigned int max );

    /// \return The value passed to SetMaxPendingForwardsPerIntermediary(), or the default.
    unsigned int GetMaxPendingForwardsPerIntermediary( void ) const;

    /// \return How many announcements SetMaxPendingForwardsPerIntermediary()'s cap has dropped.
    uint64_t GetPendingForwardsRefused( void ) const;

    /// For testing and debugging
    void SetDebugInterface( Router2DebugInterface* _debugInterface );

    /// Get the pointer passed to SetDebugInterface()
    Router2DebugInterface* GetDebugInterface( void ) const;

    // --------------------------------------------------------------------------------------------
    // Packet handling functions
    // --------------------------------------------------------------------------------------------
    virtual PluginReceiveResult OnReceive( Packet* packet );
    virtual void Update( void );
    virtual void OnClosedConnection( const SystemAddress& systemAddress, RakNetGUID rakNetGUID, PI2_LostConnectionReason lostConnectionReason );
    virtual void OnFailedConnectionAttempt( Packet* packet, PI2_FailedConnectionAttemptReason failedConnectionAttemptReason );
    virtual void OnRakPeerShutdown( void );


    enum Router2RequestStates
    {
        R2RS_REQUEST_STATE_QUERY_FORWARDING,
        REQUEST_STATE_REQUEST_FORWARDING,
    };

    struct ConnectionRequestSystem
    {
        RakNetGUID guid;
        int pingToEndpoint;
        unsigned short usedForwardingEntries;
    };

    struct ConnnectRequest
    {
        ConnnectRequest();
        ~ConnnectRequest();

        std::vector<ConnectionRequestSystem> connectionRequestSystems;
        std::mutex connectionRequestSystemsMutex;
        Router2RequestStates requestState;
        RakNet::TimeMS pingTimeout;
        RakNetGUID endpointGuid;
        RakNetGUID lastRequestedForwardingSystem;
        bool returnConnectionLostOnFailure;
        unsigned int GetGuidIndex( RakNetGUID guid );
    };

    unsigned int GetConnectionRequestIndex( RakNetGUID endpointGuid );

    struct MiniPunchRequest
    {
        RakNetGUID endpointGuid;
        SystemAddress endpointAddress;
        bool gotReplyFromEndpoint;
        RakNetGUID sourceGuid;
        SystemAddress sourceAddress;
        bool gotReplyFromSource;
        RakNet::TimeMS timeout;
        RakNet::TimeMS nextAction;
        unsigned short forwardingPort;
        __UDPSOCKET__ forwardingSocket;
    };

    struct ForwardedConnection
    {
        RakNetGUID endpointGuid;
        RakNetGUID intermediaryGuid;
        SystemAddress intermediaryAddress;
        bool returnConnectionLostOnFailure;
        bool weInitiatedForwarding;
    };

protected:
    bool UpdateForwarding( ConnnectRequest* connectionRequest );
    void RemoveConnectionRequest( unsigned int connectionRequestIndex );
    void RequestForwarding( ConnnectRequest* connectionRequest );
    void OnQueryForwarding( Packet* packet );
    void OnQueryForwardingReply( Packet* packet );
    void OnRequestForwarding( Packet* packet );
    bool OnRerouted( Packet* packet );
    void OnMiniPunchReply( Packet* packet );
    void OnMiniPunchReplyBounce( Packet* packet );
    bool OnForwardingSuccess( Packet* packet );
    int GetLargestPingAmongConnectedSystems( void ) const;
    void ReturnToUser( MessageID messageId, RakNetGUID endpointGuid, const SystemAddress& systemAddress, bool wasGeneratedLocally );
    bool ConnectInternal( RakNetGUID endpointGuid, bool returnConnectionLostOnFailure );

    UDPForwarder* udpForwarder;
    int maximumForwardingRequests;
    std::mutex connectionRequestsMutex, miniPunchesInProgressMutex, forwardedConnectionListMutex;
    std::vector<ConnnectRequest*> connectionRequests;
    std::vector<MiniPunchRequest> miniPunchesInProgress;
    // Forwarding we have initiated, and forwarding a Designated intermediary announced to us as endpoint
    std::vector<ForwardedConnection> forwardedConnectionList;
    // Guarded by forwardedConnectionListMutex
    std::vector<SystemAddress> intermediaries;
    std::atomic<unsigned int> maxPendingForwardsPerIntermediary;
    std::atomic<uint64_t> pendingForwardsRefused;

    void ClearConnectionRequests( void );
    void ClearMinipunches( void );
    void ClearForwardedConnections( void );
    void ClearAll( void );
    int ReturnFailureOnCannotForward( RakNetGUID sourceGuid, RakNetGUID endpointGuid );
    bool IsPendingForward( const ForwardedConnection& forwardedConnection, RakNetGUID intermediaryGuid );
    void SendFailureOnCannotForward( RakNetGUID sourceGuid, RakNetGUID endpointGuid );
    void SendForwardingSuccess( MessageID messageId, RakNetGUID sourceGuid, RakNetGUID endpointGuid, unsigned short sourceToDstPort );
    void SendOOBFromRakNetPort( OutOfBandIdentifiers oob, BitStream* extraData, SystemAddress sa );
    void SendOOBFromSpecifiedSocket( OutOfBandIdentifiers oob, SystemAddress sa, __UDPSOCKET__ socket );
    void SendOOBMessages( MiniPunchRequest* mpr );

    Router2DebugInterface* debugInterface;
    unsigned short socketFamily;
};

} // namespace RakNet

#endif // _RAKNET_SUPPORT_*
