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
/// \brief Contains the class RelayPlugin
///

#pragma once

#include "NativeFeatureIncludes.h"
#if _RAKNET_SUPPORT_RelayPlugin == 1

#include "PluginInterface2.h"

#include <string>
#include <unordered_map>
#include <vector>

/// \defgroup RELAY_PLUGIN_GROUP RelayPlugin
/// \brief A simple class to relay messages from one system to another through an intermediary
/// \ingroup PLUGINS_GROUP

namespace RakNet {

enum RelayPluginEnums
{
    // Server handled messages
    RPE_MESSAGE_TO_SERVER_FROM_CLIENT,
    RPE_ADD_CLIENT_REQUEST_FROM_CLIENT,
    RPE_REMOVE_CLIENT_REQUEST_FROM_CLIENT,
    RPE_GROUP_MESSAGE_FROM_CLIENT,
    RPE_JOIN_GROUP_REQUEST_FROM_CLIENT,
    RPE_LEAVE_GROUP_REQUEST_FROM_CLIENT,
    RPE_GET_GROUP_LIST_REQUEST_FROM_CLIENT,
    // Client handled messages
    RPE_MESSAGE_TO_CLIENT_FROM_SERVER,
    RPE_ADD_CLIENT_NOT_ALLOWED,
    RPE_ADD_CLIENT_TARGET_NOT_CONNECTED,
    RPE_ADD_CLIENT_NAME_ALREADY_IN_USE,
    RPE_ADD_CLIENT_SUCCESS,
    RPE_USER_ENTERED_ROOM,
    RPE_USER_LEFT_ROOM,
    RPE_GROUP_MSG_FROM_SERVER,
    RPE_GET_GROUP_LIST_REPLY_FROM_SERVER,
    RPE_JOIN_GROUP_SUCCESS,
    RPE_JOIN_GROUP_FAILURE,
};

/// \brief A simple class to relay messages from one system to another, identifying remote systems by a string.
/// \ingroup RELAY_PLUGIN_GROUP
class RAK_DLL_EXPORT RelayPlugin : public PluginInterface2
{
public:
    // GetInstance() and DestroyInstance(instance*)
    STATIC_FACTORY_DECLARATIONS( RelayPlugin )

    /// Constructor
    RelayPlugin();

    /// Destructor
    virtual ~RelayPlugin();

    /// \brief Forward messages from any system, to the system specified by the combination of key and guid. The sending system only needs to know the key.
    /// \param[in] key A string to identify the target's RakNetGUID. This is so the sending system does not need to know the RakNetGUID of the target system. The key should be unique among all guids added. If the key is not unique, only one system will be sent to (at random).
    /// \param[in] guid The RakNetGuid of the system to send to. If this system disconnects, it is removed from the internal hash
    /// \return RPE_ADD_CLIENT_TARGET_NOT_CONNECTED, RPE_ADD_CLIENT_NAME_ALREADY_IN_USE, or RPE_ADD_CLIENT_OK
    RelayPluginEnums AddParticipantOnServer( const std::string& key, const RakNetGUID& guid );

    /// \brief Remove a chat participant
    void RemoveParticipantOnServer( const RakNetGUID& guid );

    /// \brief If true, then if the client calls AddParticipantRequestFromClient(), the server will call AddParticipantOnServer() automatically
    /// Defaults to false
    /// \param[in] accept true to accept, false to not.
    void SetAcceptAddParticipantRequests( bool accept );

    /// \brief Caps the length of a participant name or group name a remote System may ask for.
    /// \details An add request with a longer name is answered with RPE_ADD_CLIENT_NOT_ALLOWED,
    /// and a join request with a longer group name with RPE_JOIN_GROUP_FAILURE. Either is counted
    /// in GetNamesRefused(). Each System is one participant, in at most one group, and a group
    /// lives only while it has a participant, so this bounds what a System can make the plugin hold.
    /// Defaults to 256 bytes.
    void SetMaxNameLength( size_t max );

    /// \return The value passed to SetMaxNameLength(), or the default.
    size_t GetMaxNameLength( void ) const;

    /// \return How many add and join requests SetMaxNameLength()'s cap has refused.
    uint64_t GetNamesRefused( void ) const;

    /// \brief Request from the client for the server to call AddParticipantOnServer()
    /// \pre The server must have called SetAcceptAddParticipantRequests(true) or the request will be ignored
    /// \param[in] key A string to identify out system. Passed to \a key on AddParticipantOnServer()
    /// \param[in] relayPluginServerGuid the RakNetGUID of the system running RelayPlugin
    void AddParticipantRequestFromClient( const std::string& key, const RakNetGUID& relayPluginServerGuid );

    /// \brief Remove yourself as a participant
    void RemoveParticipantRequestFromClient( const RakNetGUID& relayPluginServerGuid );

    /// \brief Request that the server relay \a bitStream to the system designated by \a key
    /// \param[in] relayPluginServerGuid the RakNetGUID of the system running RelayPlugin
    /// \param[in] destinationGuid The key value passed to AddParticipant() earlier on the server. If this was not done, the server will not relay the message (it will be silently discarded).
    /// \param[in] bitStream The data to relay
    /// \param[in] priority See the parameter of the same name in RakPeerInterface::Send()
    /// \param[in] reliability See the parameter of the same name in RakPeerInterface::Send()
    /// \param[in] orderingChannel See the parameter of the same name in RakPeerInterface::Send()
    void SendToParticipant( const RakNetGUID& relayPluginServerGuid, const std::string& destinationGuid, BitStream* bitStream, PacketPriority priority, PacketReliability reliability, char orderingChannel );

    void SendGroupMessage( const RakNetGUID& relayPluginServerGuid, BitStream* bitStream, PacketPriority priority, PacketReliability reliability, char orderingChannel );
    void JoinGroupRequest( const RakNetGUID& relayPluginServerGuid, const std::string& groupName );
    void LeaveGroup( const RakNetGUID& relayPluginServerGuid );
    void GetGroupList( const RakNetGUID& relayPluginServerGuid );

    /// \internal
    virtual PluginReceiveResult OnReceive( Packet* packet );
    /// \internal
    virtual void OnClosedConnection( const SystemAddress& systemAddress, RakNetGUID rakNetGUID, PI2_LostConnectionReason lostConnectionReason );

    struct StrAndGuidAndRoom
    {
        std::string str;
        RakNetGUID guid;
        std::string currentRoom;
    };

    struct StrAndGuid
    {
        std::string str;
        RakNetGUID guid;
    };

    struct RP_Group
    {
        std::string roomName;
        std::vector<StrAndGuid> usersInRoom;
    };

protected:
    RelayPlugin::RP_Group* JoinGroup( RakNetGUID userGuid, const std::string& roomName );
    RelayPlugin::RP_Group* JoinGroup( RP_Group* room, StrAndGuidAndRoom* strAndGuidSender );
    void LeaveGroup( StrAndGuidAndRoom* strAndGuidSender );
    void NotifyUsersInRoom( RP_Group* room, int msg, const std::string& message );
    void SendMessageToRoom( StrAndGuidAndRoom* strAndGuidSender, BitStream* message );
    void SendChatRoomsList( RakNetGUID target );
    void OnGroupMessageFromClient( Packet* packet );
    void OnJoinGroupRequestFromClient( Packet* packet );
    void OnLeaveGroupRequestFromClient( Packet* packet );
    void CountNameRefused( size_t length );

    std::unordered_map<std::string, StrAndGuidAndRoom*> strToGuidHash;
    std::unordered_map<RakNetGUID, StrAndGuidAndRoom*> guidToStrHash;
    std::vector<RP_Group*> chatRooms;
    bool acceptAddParticipantRequests;
    size_t maxNameLength;
    uint64_t namesRefused;
};

} // namespace RakNet

#endif // _RAKNET_SUPPORT_*
