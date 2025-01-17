/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "NativeFeatureIncludes.h"
#if _RAKNET_SUPPORT_RelayPlugin == 1

#include "Plugins/RelayPlugin.h"
#include "MessageIdentifiers.h"
#include "RakPeerInterface.h"
#include "BitStream.h"

#include <algorithm>

namespace RakNet {

STATIC_FACTORY_DEFINITIONS( RelayPlugin, RelayPlugin );

RelayPlugin::RelayPlugin()
{
    acceptAddParticipantRequests = false;
}

RelayPlugin::~RelayPlugin()
{
    guidToStrHash.clear();
    for( const auto& entry : strToGuidHash )
    {
        RakNet::OP_DELETE( entry.second, _FILE_AND_LINE_ );
    }
    for( RP_Group* pRoom : chatRooms )
    {
        RakNet::OP_DELETE( pRoom, _FILE_AND_LINE_ );
    }
}

RelayPluginEnums RelayPlugin::AddParticipantOnServer( const std::string& key, const RakNetGUID& guid )
{
    ConnectionState cs = rakPeerInterface->GetConnectionState( guid );
    if( cs != IS_CONNECTED )
        return RPE_ADD_CLIENT_TARGET_NOT_CONNECTED;

    if( strToGuidHash.find( key ) != strToGuidHash.end() )
        return RPE_ADD_CLIENT_NAME_ALREADY_IN_USE; // Name already in use

    // If GUID is already in use, remove existing
    if( auto it = guidToStrHash.find( guid ); it != guidToStrHash.end() )
    {
        StrAndGuidAndRoom* strAndGuid = it->second;
        strToGuidHash.erase( strAndGuid->str );
        guidToStrHash.erase( it );
        RakNet::OP_DELETE( strAndGuid, _FILE_AND_LINE_ );
    }

    StrAndGuidAndRoom* strAndGuid = RakNet::OP_NEW<StrAndGuidAndRoom>( _FILE_AND_LINE_ );
    strAndGuid->guid = guid;
    strAndGuid->str = key;

    strToGuidHash.insert( std::make_pair( key, strAndGuid ) );
    guidToStrHash.insert( std::make_pair( guid, strAndGuid ) );

    return RPE_ADD_CLIENT_SUCCESS;
}

void RelayPlugin::RemoveParticipantOnServer( const RakNetGUID& guid )
{
    if( auto it = guidToStrHash.find( guid ); it != guidToStrHash.end() )
    {
        StrAndGuidAndRoom* strAndGuid = it->second;
        LeaveGroup( strAndGuid );
        strToGuidHash.erase( strAndGuid->str );
        guidToStrHash.erase( it );
        RakNet::OP_DELETE( strAndGuid, _FILE_AND_LINE_ );
    }
}

void RelayPlugin::SetAcceptAddParticipantRequests( bool accept )
{
    acceptAddParticipantRequests = accept;
}
void RelayPlugin::AddParticipantRequestFromClient( const std::string& key, const RakNetGUID& relayPluginServerGuid )
{
    BitStream bsOut;
    bsOut.WriteCasted<MessageID>( ID_RELAY_PLUGIN );
    bsOut.WriteCasted<MessageID>( RPE_ADD_CLIENT_REQUEST_FROM_CLIENT );
    bsOut.WriteCompressed( key );
    SendUnified( &bsOut, HIGH_PRIORITY, RELIABLE_ORDERED, 0, relayPluginServerGuid, false );
}
void RelayPlugin::RemoveParticipantRequestFromClient( const RakNetGUID& relayPluginServerGuid )
{
    BitStream bsOut;
    bsOut.WriteCasted<MessageID>( ID_RELAY_PLUGIN );
    bsOut.WriteCasted<MessageID>( RPE_REMOVE_CLIENT_REQUEST_FROM_CLIENT );
    SendUnified( &bsOut, HIGH_PRIORITY, RELIABLE_ORDERED, 0, relayPluginServerGuid, false );
}
// Send a message to a server running RelayPlugin, to forward a message to the system identified by \a key
void RelayPlugin::SendToParticipant( const RakNetGUID& relayPluginServerGuid, const std::string& key, BitStream* bitStream, PacketPriority priority, PacketReliability reliability, char orderingChannel )
{
    BitStream bsOut;
    bsOut.WriteCasted<MessageID>( ID_RELAY_PLUGIN );
    bsOut.WriteCasted<MessageID>( RPE_MESSAGE_TO_SERVER_FROM_CLIENT );
    bsOut.WriteCasted<unsigned char>( priority );
    bsOut.WriteCasted<unsigned char>( reliability );
    bsOut.Write( orderingChannel );
    bsOut.WriteCompressed( key );
    bsOut.Write( bitStream );
    SendUnified( &bsOut, priority, reliability, orderingChannel, relayPluginServerGuid, false );
}
void RelayPlugin::SendGroupMessage( const RakNetGUID& relayPluginServerGuid, BitStream* bitStream, PacketPriority priority, PacketReliability reliability, char orderingChannel )
{
    BitStream bsOut;
    bsOut.WriteCasted<MessageID>( ID_RELAY_PLUGIN );
    bsOut.WriteCasted<MessageID>( RPE_GROUP_MESSAGE_FROM_CLIENT );
    bsOut.WriteCasted<unsigned char>( priority );
    bsOut.WriteCasted<unsigned char>( reliability );
    bsOut.Write( orderingChannel );
    bsOut.Write( bitStream );
    SendUnified( &bsOut, priority, reliability, orderingChannel, relayPluginServerGuid, false );
}
void RelayPlugin::LeaveGroup( const RakNetGUID& relayPluginServerGuid )
{
    BitStream bsOut;
    bsOut.WriteCasted<MessageID>( ID_RELAY_PLUGIN );
    bsOut.WriteCasted<MessageID>( RPE_LEAVE_GROUP_REQUEST_FROM_CLIENT );
    SendUnified( &bsOut, HIGH_PRIORITY, RELIABLE_ORDERED, 0, relayPluginServerGuid, false );
}
void RelayPlugin::GetGroupList( const RakNetGUID& relayPluginServerGuid )
{
    BitStream bsOut;
    bsOut.WriteCasted<MessageID>( ID_RELAY_PLUGIN );
    bsOut.WriteCasted<MessageID>( RPE_GET_GROUP_LIST_REQUEST_FROM_CLIENT );
    SendUnified( &bsOut, HIGH_PRIORITY, RELIABLE_ORDERED, 0, relayPluginServerGuid, false );
}
PluginReceiveResult RelayPlugin::OnReceive( Packet* packet )
{
    if( packet->data[0] == ID_RELAY_PLUGIN )
    {
        switch( packet->data[1] )
        {
        case RPE_MESSAGE_TO_SERVER_FROM_CLIENT: {
            BitStream bsIn( packet->data, packet->length, false );
            bsIn.IgnoreBytes( sizeof( MessageID ) * 2 );
            PacketPriority priority;
            PacketReliability reliability;
            char orderingChannel;
            unsigned char cIn;
            bsIn.Read( cIn );
            priority = (PacketPriority)cIn;
            bsIn.Read( cIn );
            reliability = (PacketReliability)cIn;
            bsIn.Read( orderingChannel );
            std::string key;
            bsIn.ReadCompressed( key );
            BitStream bsData;
            bsIn.Read( &bsData );
            auto itStrAndGuid = strToGuidHash.find( key );
            auto itStrAndGuidSender = guidToStrHash.find( packet->guid );
            if( itStrAndGuid != strToGuidHash.end() && itStrAndGuidSender != guidToStrHash.end() )
            {
                BitStream bsOut;
                bsOut.WriteCasted<MessageID>( ID_RELAY_PLUGIN );
                bsOut.WriteCasted<MessageID>( RPE_MESSAGE_TO_CLIENT_FROM_SERVER );
                bsOut.WriteCompressed( itStrAndGuidSender->second->str );
                bsOut.AlignWriteToByteBoundary();
                bsOut.Write( bsData );
                SendUnified( &bsOut, priority, reliability, orderingChannel, itStrAndGuid->second->guid, false );
            }

            return RR_STOP_PROCESSING_AND_DEALLOCATE;
        }

        case RPE_ADD_CLIENT_REQUEST_FROM_CLIENT: {
            BitStream bsIn( packet->data, packet->length, false );
            bsIn.IgnoreBytes( sizeof( MessageID ) * 2 );
            std::string key;
            bsIn.ReadCompressed( key );
            BitStream bsOut;
            bsOut.WriteCasted<MessageID>( ID_RELAY_PLUGIN );
            if( acceptAddParticipantRequests )
                bsOut.WriteCasted<MessageID>( AddParticipantOnServer( key, packet->guid ) );
            else
                bsOut.WriteCasted<MessageID>( RPE_ADD_CLIENT_NOT_ALLOWED );
            bsOut.WriteCompressed( key );
            SendUnified( &bsOut, HIGH_PRIORITY, RELIABLE_ORDERED, 0, packet->systemAddress, false );

            return RR_STOP_PROCESSING_AND_DEALLOCATE;
        }
        case RPE_REMOVE_CLIENT_REQUEST_FROM_CLIENT: {
            RemoveParticipantOnServer( packet->guid );
        }
            return RR_STOP_PROCESSING_AND_DEALLOCATE;
        case RPE_GROUP_MESSAGE_FROM_CLIENT: {
            OnGroupMessageFromClient( packet );
        }
            return RR_STOP_PROCESSING_AND_DEALLOCATE;
        case RPE_JOIN_GROUP_REQUEST_FROM_CLIENT: {
            OnJoinGroupRequestFromClient( packet );
        }
            return RR_STOP_PROCESSING_AND_DEALLOCATE;
        case RPE_LEAVE_GROUP_REQUEST_FROM_CLIENT: {
            OnLeaveGroupRequestFromClient( packet );
        }
            return RR_STOP_PROCESSING_AND_DEALLOCATE;
        case RPE_GET_GROUP_LIST_REQUEST_FROM_CLIENT: {
            SendChatRoomsList( packet->guid );
        }
            return RR_STOP_PROCESSING_AND_DEALLOCATE;
        }
    }

    return RR_CONTINUE_PROCESSING;
}

void RelayPlugin::OnClosedConnection( const SystemAddress& systemAddress, RakNetGUID rakNetGUID, PI2_LostConnectionReason lostConnectionReason )
{
    (void)lostConnectionReason;
    (void)systemAddress;

    RemoveParticipantOnServer( rakNetGUID );
}

RelayPlugin::RP_Group* RelayPlugin::JoinGroup( RP_Group* room, StrAndGuidAndRoom* strAndGuidSender )
{
    if( strAndGuidSender == 0 )
        return 0;

    NotifyUsersInRoom( room, RPE_USER_ENTERED_ROOM, strAndGuidSender->str );

    room->usersInRoom.emplace_back( StrAndGuid{ strAndGuidSender->str, strAndGuidSender->guid } );
    strAndGuidSender->currentRoom = room->roomName;

    return room;
}

void RelayPlugin::JoinGroupRequest( const RakNetGUID& relayPluginServerGuid, const std::string& groupName )
{
    BitStream bsOut;
    bsOut.WriteCasted<MessageID>( ID_RELAY_PLUGIN );
    bsOut.WriteCasted<MessageID>( RPE_JOIN_GROUP_REQUEST_FROM_CLIENT );
    bsOut.WriteCompressed( groupName );
    SendUnified( &bsOut, HIGH_PRIORITY, RELIABLE_ORDERED, 0, relayPluginServerGuid, false );
}

RelayPlugin::RP_Group* RelayPlugin::JoinGroup( RakNetGUID userGuid, const std::string& roomName )
{
    if( auto it = guidToStrHash.find( userGuid ); it != guidToStrHash.end() )
    {
        StrAndGuidAndRoom* strAndGuidSender = it->second;
        if( roomName.empty() )
            return 0;

        if( strAndGuidSender->currentRoom == roomName )
            return 0;

        if( !strAndGuidSender->currentRoom.empty() )
            LeaveGroup( strAndGuidSender );

        for( RP_Group* pRoom : chatRooms )
        {
            if( pRoom->roomName == roomName )
            {
                // Join existing room
                return JoinGroup( pRoom, strAndGuidSender );
            }
        }

        // Create new room
        RP_Group* room = RakNet::OP_NEW<RP_Group>( _FILE_AND_LINE_ );
        room->roomName = roomName;
        chatRooms.push_back( room );

        return JoinGroup( room, strAndGuidSender );
    }

    return 0;
}

void RelayPlugin::LeaveGroup( StrAndGuidAndRoom* strAndGuidSender )
{
    if( strAndGuidSender == 0 )
        return;

    const std::string& userName = strAndGuidSender->str;

    auto itRoom = std::find_if( chatRooms.begin(), chatRooms.end(),
                                [strAndGuidSender]( const RP_Group* pRoom ) { return pRoom->roomName == strAndGuidSender->currentRoom; } );
    if( itRoom != chatRooms.end() )
    {
        strAndGuidSender->currentRoom.clear();

        RP_Group* room = *itRoom;
        for( auto itUser = room->usersInRoom.begin(); itUser != room->usersInRoom.end(); /**/ )
        {
            if( itUser->guid == strAndGuidSender->guid )
            {
                itUser = room->usersInRoom.erase( itUser );

                if( room->usersInRoom.empty() )
                {
                    RakNet::OP_DELETE( room, _FILE_AND_LINE_ );
                    chatRooms.erase( itRoom );
                    return;
                }
            }
            else
            {
                ++itUser;
            }
        }

        NotifyUsersInRoom( room, RPE_USER_LEFT_ROOM, userName );

        return;
    }
}

void RelayPlugin::NotifyUsersInRoom( RP_Group* room, int msg, const std::string& message )
{
    for( const StrAndGuid& rUser : room->usersInRoom )
    {
        BitStream bsOut;
        bsOut.WriteCasted<MessageID>( ID_RELAY_PLUGIN );
        bsOut.WriteCasted<MessageID>( msg );
        bsOut.WriteCompressed( message );

        SendUnified( &bsOut, HIGH_PRIORITY, RELIABLE_ORDERED, 0, rUser.guid, false );
    }
}

void RelayPlugin::SendMessageToRoom( StrAndGuidAndRoom* strAndGuidSender, BitStream* message )
{
    if( strAndGuidSender->currentRoom.empty() )
        return;

    for( const RP_Group* pRoom : chatRooms )
    {
        if( pRoom->roomName == strAndGuidSender->currentRoom )
        {
            BitStream bsOut;
            bsOut.WriteCasted<MessageID>( ID_RELAY_PLUGIN );
            bsOut.WriteCasted<MessageID>( RPE_GROUP_MSG_FROM_SERVER );
            message->ResetReadPointer();
            bsOut.WriteCompressed( strAndGuidSender->str );
            bsOut.AlignWriteToByteBoundary();
            bsOut.Write( message );

            for( const StrAndGuid& rUser : pRoom->usersInRoom )
            {
                if( rUser.guid != strAndGuidSender->guid )
                {
                    SendUnified( &bsOut, HIGH_PRIORITY, RELIABLE_ORDERED, 0, rUser.guid, false );
                }
            }

            break;
        }
    }
}

void RelayPlugin::SendChatRoomsList( RakNetGUID target )
{
    BitStream bsOut;
    bsOut.WriteCasted<MessageID>( ID_RELAY_PLUGIN );
    bsOut.WriteCasted<MessageID>( RPE_GET_GROUP_LIST_REPLY_FROM_SERVER );
    bsOut.WriteCasted<uint16_t>( static_cast<uint16_t>( chatRooms.size() ) );
    for( const RP_Group* pRoom : chatRooms )
    {
        bsOut.WriteCompressed( pRoom->roomName );
        bsOut.WriteCasted<uint16_t>( static_cast<uint16_t>( pRoom->usersInRoom.size() ) );
    }
    SendUnified( &bsOut, HIGH_PRIORITY, RELIABLE_ORDERED, 0, target, false );
}

void RelayPlugin::OnGroupMessageFromClient( Packet* packet )
{
    BitStream bsIn( packet->data, packet->length, false );
    bsIn.IgnoreBytes( sizeof( MessageID ) * 2 );

    PacketPriority priority;
    PacketReliability reliability;
    char orderingChannel;
    unsigned char cIn;
    bsIn.Read( cIn );
    priority = (PacketPriority)cIn;
    bsIn.Read( cIn );
    reliability = (PacketReliability)cIn;
    bsIn.Read( orderingChannel );
    BitStream bsData;
    bsIn.Read( &bsData );

    if( auto it = guidToStrHash.find( packet->guid ); it != guidToStrHash.end() )
    {
        SendMessageToRoom( it->second, &bsData );
    }
}

void RelayPlugin::OnJoinGroupRequestFromClient( Packet* packet )
{
    BitStream bsIn( packet->data, packet->length, false );
    bsIn.IgnoreBytes( sizeof( MessageID ) * 2 );
    std::string groupName;
    bsIn.ReadCompressed( groupName );
    RelayPlugin::RP_Group* groupJoined = JoinGroup( packet->guid, groupName );

    BitStream bsOut;
    bsOut.WriteCasted<MessageID>( ID_RELAY_PLUGIN );
    if( groupJoined )
    {
        bsOut.WriteCasted<MessageID>( RPE_JOIN_GROUP_SUCCESS );
        bsOut.WriteCasted<uint16_t>( static_cast<uint16_t>( groupJoined->usersInRoom.size() ) );
        for( const StrAndGuid& rUser : groupJoined->usersInRoom )
        {
            bsOut.WriteCompressed( rUser.str );
        }
    }
    else
    {
        bsOut.WriteCasted<MessageID>( RPE_JOIN_GROUP_FAILURE );
    }

    SendUnified( &bsOut, HIGH_PRIORITY, RELIABLE_ORDERED, 0, packet->guid, false );
}

void RelayPlugin::OnLeaveGroupRequestFromClient( Packet* packet )
{
    BitStream bsIn( packet->data, packet->length, false );
    bsIn.IgnoreBytes( sizeof( MessageID ) * 2 );
    if( auto it = guidToStrHash.find( packet->guid ); it != guidToStrHash.end() )
    {
        LeaveGroup( it->second );
    }
}

} // namespace RakNet

#endif // _RAKNET_SUPPORT_*
