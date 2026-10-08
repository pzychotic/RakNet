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
#if _RAKNET_SUPPORT_NatPunchthroughServer == 1

#include "Plugins/NatPunchthroughServer.h"
#include "Plugins/PacketLogger.h"
#include "StringUtils.h"
#include "BitStream.h"
#include "MessageIdentifiers.h"
#include "RakPeerInterface.h"
#include "MTUSize.h"
#include "GetTime.h"

#include <algorithm>

namespace RakNet {

void NatPunchthroughServerDebugInterface_Printf::OnServerMessage( const char* msg )
{
    printf( "%s\n", msg );
}
#if _RAKNET_SUPPORT_PacketLogger == 1
void NatPunchthroughServerDebugInterface_PacketLogger::OnServerMessage( const char* msg )
{
    if( pl )
    {
        pl->WriteMiscellaneous( "Nat", msg );
    }
}
#endif

void NatPunchthroughServer::User::DeleteConnectionAttempt( NatPunchthroughServer::ConnectionAttempt* ca )
{
    auto it = std::find( connectionAttempts.begin(), connectionAttempts.end(), ca );
    if( it != connectionAttempts.end() )
    {
        RakNet::OP_DELETE( ca, _FILE_AND_LINE_ );
        connectionAttempts.erase( it );
    }
}

void NatPunchthroughServer::User::DerefConnectionAttempt( NatPunchthroughServer::ConnectionAttempt* ca )
{
    auto it = std::find( connectionAttempts.begin(), connectionAttempts.end(), ca );
    if( it != connectionAttempts.end() )
    {
        connectionAttempts.erase( it );
    }
}

std::vector<NatPunchthroughServer::ConnectionAttempt*> NatPunchthroughServer::User::TakeConnectionAttempts()
{
    std::vector<ConnectionAttempt*> attempts;
    attempts.swap( connectionAttempts );

    // An attempt from this User to itself is on its list twice. OnNATPunchthroughRequest no
    // longer creates one, but a second entry would be deleted twice.
    for( auto it = attempts.begin(); it != attempts.end(); ++it )
    {
        if( *it != nullptr && ( *it )->sender == ( *it )->recipient )
            std::replace( it + 1, attempts.end(), *it, static_cast<ConnectionAttempt*>( nullptr ) );
    }
    attempts.erase( std::remove( attempts.begin(), attempts.end(), nullptr ), attempts.end() );
    return attempts;
}

bool NatPunchthroughServer::User::HasConnectionAttemptToUser( User* user )
{
    auto it = std::find_if( connectionAttempts.begin(), connectionAttempts.end(),
                            [user]( const ConnectionAttempt* pAttempt ) { return pAttempt->recipient->guid == user->guid || pAttempt->sender->guid == user->guid; } );
    return it != connectionAttempts.end();
}

void NatPunchthroughServer::User::LogConnectionAttempts( std::string& rs )
{
    rs.clear();
    const uint32_t uAttemptsCount = static_cast<uint32_t>( connectionAttempts.size() );
    char guidStr[128], ipStr[128];
    guid.ToString( guidStr );
    systemAddress.ToString( true, ipStr );
    rs = RakNet::format( "User systemAddress=%s guid=%s\n", ipStr, guidStr );
    rs += RakNet::format( "%u attempts in list:\n", uAttemptsCount );
    for( uint32_t index = 0; index < uAttemptsCount; ++index )
    {
        const ConnectionAttempt* pConnectionAttempt = connectionAttempts[index];

        rs += RakNet::format( "%u. SessionID=%i ", index + 1, pConnectionAttempt->sessionId );
        rs += pConnectionAttempt->sender == this ? "(We are sender) " : "(We are recipient) ";
        rs += isReady ? "(READY TO START) " : "(NOT READY TO START) ";
        rs += pConnectionAttempt->attemptPhase == NatPunchthroughServer::ConnectionAttempt::NAT_ATTEMPT_PHASE_NOT_STARTED ? "(NOT_STARTED). " : "(GETTING_RECENT_PORTS). ";

        const NatPunchthroughServer::User* pUser = pConnectionAttempt->sender == this ? pConnectionAttempt->recipient : pConnectionAttempt->sender;
        pUser->guid.ToString( guidStr );
        pUser->systemAddress.ToString( true, ipStr );

        rs += RakNet::format( "Target systemAddress=%s, guid=%s.\n", ipStr, guidStr );
    }
}

STATIC_FACTORY_DEFINITIONS( NatPunchthroughServer, NatPunchthroughServer );

NatPunchthroughServer::NatPunchthroughServer()
{
    lastUpdate = 0;
    sessionId = 0;
    natPunchthroughServerDebugInterface = 0;
    for( int i = 0; i < MAXIMUM_NUMBER_OF_INTERNAL_IDS; i++ )
        boundAddresses[i] = UNASSIGNED_SYSTEM_ADDRESS;
    boundAddressCount = 0;
}

NatPunchthroughServer::~NatPunchthroughServer()
{
    while( !users.empty() )
    {
        User* user = users.begin()->second;
        for( ConnectionAttempt* pConnectionAttempt : user->TakeConnectionAttempts() )
        {
            User* otherUser = pConnectionAttempt->sender == user ? pConnectionAttempt->recipient : pConnectionAttempt->sender;
            otherUser->DerefConnectionAttempt( pConnectionAttempt );
            RakNet::OP_DELETE( pConnectionAttempt, _FILE_AND_LINE_ );
        }
        RakNet::OP_DELETE( user, _FILE_AND_LINE_ );
        users.erase( users.begin() );
    }
}

void NatPunchthroughServer::SetDebugInterface( NatPunchthroughServerDebugInterface* i )
{
    natPunchthroughServerDebugInterface = i;
}

void NatPunchthroughServer::Update( void )
{
    RakNet::Time time = RakNet::GetTime();
    if( time > lastUpdate + 250 )
    {
        lastUpdate = time;

        for( const auto& [guid, user] : users )
        {
            for( ConnectionAttempt* connectionAttempt : user->connectionAttempts )
            {
                if( connectionAttempt->sender == user )
                {
                    if( connectionAttempt->attemptPhase != ConnectionAttempt::NAT_ATTEMPT_PHASE_NOT_STARTED &&
                        time > connectionAttempt->startTime &&
                        time > 10000 + connectionAttempt->startTime ) // Formerly 5000, but sometimes false positives
                    {
                        BitStream outgoingBs;

                        // that other system might not be running the plugin
                        outgoingBs.Write( (MessageID)ID_NAT_TARGET_UNRESPONSIVE );
                        outgoingBs.Write( connectionAttempt->recipient->guid );
                        outgoingBs.Write( connectionAttempt->sessionId );
                        rakPeerInterface->Send( &outgoingBs, HIGH_PRIORITY, RELIABLE_ORDERED, 0, connectionAttempt->sender->systemAddress, false );

                        // 05/28/09 Previously only told sender about ID_NAT_CONNECTION_TO_TARGET_LOST
                        // However, recipient may be expecting it due to external code
                        // In that case, recipient would never get any response if the sender dropped
                        outgoingBs.Reset();
                        outgoingBs.Write( (MessageID)ID_NAT_TARGET_UNRESPONSIVE );
                        outgoingBs.Write( connectionAttempt->sender->guid );
                        outgoingBs.Write( connectionAttempt->sessionId );
                        rakPeerInterface->Send( &outgoingBs, HIGH_PRIORITY, RELIABLE_ORDERED, 0, connectionAttempt->recipient->systemAddress, false );

                        connectionAttempt->sender->isReady = true;
                        connectionAttempt->recipient->isReady = true;
                        User* recipient = connectionAttempt->recipient;

                        if( natPunchthroughServerDebugInterface )
                        {
                            char addr1[128], addr2[128];
                            // 8/01/09 Fixed bug where this was after DeleteConnectionAttempt()
                            connectionAttempt->sender->systemAddress.ToString( true, addr1 );
                            connectionAttempt->recipient->systemAddress.ToString( true, addr2 );
                            std::string str = RakNet::format( "Sending ID_NAT_TARGET_UNRESPONSIVE to sender %s and recipient %s.", addr1, addr2 );
                            natPunchthroughServerDebugInterface->OnServerMessage( str.c_str() );
                            std::string log;
                            connectionAttempt->sender->LogConnectionAttempts( log );
                            connectionAttempt->recipient->LogConnectionAttempts( log );
                        }

                        connectionAttempt->sender->DerefConnectionAttempt( connectionAttempt );
                        connectionAttempt->recipient->DeleteConnectionAttempt( connectionAttempt );

                        StartPunchthroughForUser( user );
                        StartPunchthroughForUser( recipient );

                        break;
                    }
                }
            }
        }
    }
}
PluginReceiveResult NatPunchthroughServer::OnReceive( Packet* packet )
{
    switch( packet->data[0] )
    {
    case ID_NAT_PUNCHTHROUGH_REQUEST:
        OnNATPunchthroughRequest( packet );
        return RR_STOP_PROCESSING_AND_DEALLOCATE;
    case ID_NAT_GET_MOST_RECENT_PORT:
        OnGetMostRecentPort( packet );
        return RR_STOP_PROCESSING_AND_DEALLOCATE;
    case ID_NAT_CLIENT_READY:
        OnClientReady( packet );
        return RR_STOP_PROCESSING_AND_DEALLOCATE;
    case ID_NAT_REQUEST_BOUND_ADDRESSES: {
        BitStream outgoingBs;
        outgoingBs.Write( (MessageID)ID_NAT_RESPOND_BOUND_ADDRESSES );

        if( boundAddresses[0] == UNASSIGNED_SYSTEM_ADDRESS )
        {
            std::vector<RakNetSocket2*> sockets;
            rakPeerInterface->GetSockets( sockets );
            for( size_t i = 0; i < sockets.size() && i < MAXIMUM_NUMBER_OF_INTERNAL_IDS; ++i )
            {
                boundAddresses[i] = sockets[i]->GetBoundAddress();
                boundAddressCount++;
            }
        }

        outgoingBs.Write( boundAddressCount );
        for( int i = 0; i < boundAddressCount; i++ )
        {
            outgoingBs.Write( boundAddresses[i] );
        }

        rakPeerInterface->Send( &outgoingBs, HIGH_PRIORITY, RELIABLE_ORDERED, 0, packet->systemAddress, false );
    }
        return RR_STOP_PROCESSING_AND_DEALLOCATE;
    case ID_NAT_PING: {
    }
        return RR_STOP_PROCESSING_AND_DEALLOCATE;
    case ID_OUT_OF_BAND_INTERNAL:
        if( packet->length >= 2 && packet->data[1] == ID_NAT_PING )
        {
            BitStream bs( packet->data, packet->length, false );
            bs.IgnoreBytes( sizeof( MessageID ) * 2 );
            uint16_t externalPort;
            bs.Read( externalPort );

            BitStream outgoingBs;
            outgoingBs.Write( (MessageID)ID_NAT_PONG );
            outgoingBs.Write( externalPort );
            uint16_t externalPort2 = packet->systemAddress.GetPort();
            outgoingBs.Write( externalPort2 );
            rakPeerInterface->SendOutOfBand( (const char*)packet->systemAddress.ToString( false ), packet->systemAddress.GetPort(), (const char*)outgoingBs.GetData(), outgoingBs.GetNumberOfBytesUsed() );

            return RR_STOP_PROCESSING_AND_DEALLOCATE;
        }
    }
    return RR_CONTINUE_PROCESSING;
}
void NatPunchthroughServer::OnClosedConnection( const SystemAddress& systemAddress, RakNetGUID rakNetGUID, PI2_LostConnectionReason lostConnectionReason )
{
    (void)lostConnectionReason;
    (void)systemAddress;

    const auto userIt = users.find( rakNetGUID );
    if( userIt != users.end() )
    {
        BitStream outgoingBs;
        std::vector<User*> freedUpInProgressUsers;
        User* user = userIt->second;
        for( ConnectionAttempt* connectionAttempt : user->TakeConnectionAttempts() )
        {
            outgoingBs.Reset();
            User* otherUser = connectionAttempt->recipient == user ? connectionAttempt->sender : connectionAttempt->recipient;

            // An attempt to itself has no one else to tell, and user is about to be deleted.
            if( otherUser == user )
            {
                RakNet::OP_DELETE( connectionAttempt, _FILE_AND_LINE_ );
                continue;
            }

            // 05/28/09 Previously only told sender about ID_NAT_CONNECTION_TO_TARGET_LOST
            // However, recipient may be expecting it due to external code
            // In that case, recipient would never get any response if the sender dropped
            outgoingBs.Write( (MessageID)ID_NAT_CONNECTION_TO_TARGET_LOST );
            outgoingBs.Write( rakNetGUID );
            outgoingBs.Write( connectionAttempt->sessionId );
            rakPeerInterface->Send( &outgoingBs, HIGH_PRIORITY, RELIABLE_ORDERED, 0, otherUser->systemAddress, false );

            // 4/22/09 - Bug: was checking inProgress, legacy variable not used elsewhere
            if( connectionAttempt->attemptPhase == ConnectionAttempt::NAT_ATTEMPT_PHASE_GETTING_RECENT_PORTS )
            {
                otherUser->isReady = true;
                freedUpInProgressUsers.push_back( otherUser );
            }

            otherUser->DerefConnectionAttempt( connectionAttempt );
            RakNet::OP_DELETE( connectionAttempt, _FILE_AND_LINE_ );
        }

        RakNet::OP_DELETE( user, _FILE_AND_LINE_ );
        users.erase( userIt );

        for( User* pUser : freedUpInProgressUsers )
        {
            StartPunchthroughForUser( pUser );
        }
    }
}

void NatPunchthroughServer::OnNewConnection( const SystemAddress& systemAddress, RakNetGUID rakNetGUID, bool isIncoming )
{
    (void)systemAddress;
    (void)isIncoming;

    User* user = RakNet::OP_NEW<User>( _FILE_AND_LINE_ );
    user->guid = rakNetGUID;
    user->mostRecentPort = 0;
    user->systemAddress = systemAddress;
    user->isReady = true;
    // A guid already listed keeps its User.
    if( !users.emplace( rakNetGUID, user ).second )
        RakNet::OP_DELETE( user, _FILE_AND_LINE_ );
}

void NatPunchthroughServer::OnNATPunchthroughRequest( Packet* packet )
{
    BitStream outgoingBs;
    BitStream incomingBs( packet->data, packet->length, false );
    incomingBs.IgnoreBytes( sizeof( MessageID ) );
    RakNetGUID recipientGuid, senderGuid;
    incomingBs.Read( recipientGuid );
    senderGuid = packet->guid;
    const auto senderIt = users.find( senderGuid );
    // A sender that connected before this plugin was attached has no User.
    if( senderIt == users.end() )
        return;
    User* sender = senderIt->second;

    const auto recipientIt = users.find( recipientGuid );
    // A request to the sender's own guid is refused too: its attempt would be listed twice on
    // one User.
    if( recipientIt == users.end() || recipientGuid == senderGuid )
    {
        outgoingBs.Write( (MessageID)ID_NAT_TARGET_NOT_CONNECTED );
        outgoingBs.Write( recipientGuid );
        rakPeerInterface->Send( &outgoingBs, HIGH_PRIORITY, RELIABLE_ORDERED, 0, packet->systemAddress, false );
        return;
    }
    User* recipient = recipientIt->second;
    if( recipient->HasConnectionAttemptToUser( sender ) )
    {
        outgoingBs.Write( (MessageID)ID_NAT_ALREADY_IN_PROGRESS );
        outgoingBs.Write( recipientGuid );
        rakPeerInterface->Send( &outgoingBs, HIGH_PRIORITY, RELIABLE_ORDERED, 0, packet->systemAddress, false );
        return;
    }

    ConnectionAttempt* ca = RakNet::OP_NEW<ConnectionAttempt>( _FILE_AND_LINE_ );
    ca->sender = sender;
    ca->recipient = recipient;
    ca->sessionId = sessionId++;
    ca->sender->connectionAttempts.push_back( ca );
    ca->recipient->connectionAttempts.push_back( ca );

    StartPunchthroughForUser( ca->sender );
}

void NatPunchthroughServer::OnClientReady( Packet* packet )
{
    const auto userIt = users.find( packet->guid );
    if( userIt != users.end() )
    {
        userIt->second->isReady = true;
        StartPunchthroughForUser( userIt->second );
    }
}

void NatPunchthroughServer::OnGetMostRecentPort( Packet* packet )
{
    BitStream bsIn( packet->data, packet->length, false );
    bsIn.IgnoreBytes( sizeof( MessageID ) );
    uint16_t sessionId;
    unsigned short mostRecentPort;
    bsIn.Read( sessionId );
    bsIn.Read( mostRecentPort );

    const auto userIt = users.find( packet->guid );
    const bool objectExists = userIt != users.end();

    if( natPunchthroughServerDebugInterface )
    {
        char addr1[128], addr2[128];
        packet->systemAddress.ToString( true, addr1 );
        packet->guid.ToString( addr2 );
        std::string log = RakNet::format( "Got ID_NAT_GET_MOST_RECENT_PORT from systemAddress %s guid %s. port=%i. sessionId=%i. userFound=%i.", addr1, addr2, mostRecentPort, sessionId, objectExists );
        natPunchthroughServerDebugInterface->OnServerMessage( log.c_str() );
    }

    if( objectExists )
    {
        User* user = userIt->second;
        user->mostRecentPort = mostRecentPort;
        RakNet::Time time = RakNet::GetTime();

        for( ConnectionAttempt* connectionAttempt : user->connectionAttempts )
        {
            if( connectionAttempt->attemptPhase == ConnectionAttempt::NAT_ATTEMPT_PHASE_GETTING_RECENT_PORTS &&
                connectionAttempt->sender->mostRecentPort != 0 &&
                connectionAttempt->recipient->mostRecentPort != 0 &&
                // 04/29/08 add sessionId to prevent processing for other systems
                connectionAttempt->sessionId == sessionId )
            {
                SystemAddress senderSystemAddress = connectionAttempt->sender->systemAddress;
                SystemAddress recipientSystemAddress = connectionAttempt->recipient->systemAddress;
                SystemAddress recipientTargetAddress = recipientSystemAddress;
                SystemAddress senderTargetAddress = senderSystemAddress;
                recipientTargetAddress.SetPortHostOrder( connectionAttempt->recipient->mostRecentPort );
                senderTargetAddress.SetPortHostOrder( connectionAttempt->sender->mostRecentPort );

                // Pick a time far enough in the future that both systems will have gotten the message
                int targetPing = rakPeerInterface->GetAveragePing( recipientTargetAddress );
                int senderPing = rakPeerInterface->GetAveragePing( senderSystemAddress );
                RakNet::Time simultaneousAttemptTime;
                if( targetPing == -1 || senderPing == -1 )
                {
                    simultaneousAttemptTime = time + 1500;
                }
                else
                {
                    int largerPing = targetPing > senderPing ? targetPing : senderPing;
                    simultaneousAttemptTime = time + ( ( largerPing * 4 < 100 ) ? 100 : ( largerPing * 4 ) );
                }

                if( natPunchthroughServerDebugInterface )
                {
                    char addr1[128], addr2[128];
                    recipientSystemAddress.ToString( true, addr1 );
                    connectionAttempt->recipient->guid.ToString( addr2 );
                    std::string log = RakNet::format( "Sending ID_NAT_CONNECT_AT_TIME to recipient systemAddress %s guid %s", addr1, addr2 );
                    natPunchthroughServerDebugInterface->OnServerMessage( log.c_str() );
                }

                // Send to recipient timestamped message to connect at time
                BitStream bsOut;
                bsOut.Write( (MessageID)ID_TIMESTAMP );
                bsOut.Write( simultaneousAttemptTime );
                bsOut.Write( (MessageID)ID_NAT_CONNECT_AT_TIME );
                bsOut.Write( connectionAttempt->sessionId );
                bsOut.Write( senderTargetAddress );                   // Public IP, using most recent port
                for( int j = 0; j < MAXIMUM_NUMBER_OF_INTERNAL_IDS; j++ ) // Internal IP
                    bsOut.Write( rakPeerInterface->GetInternalID( senderSystemAddress, j ) );
                bsOut.Write( connectionAttempt->sender->guid );
                bsOut.Write( false );
                rakPeerInterface->Send( &bsOut, HIGH_PRIORITY, RELIABLE_ORDERED, 0, recipientSystemAddress, false );


                if( natPunchthroughServerDebugInterface )
                {
                    char addr1[128], addr2[128];
                    senderSystemAddress.ToString( true, addr1 );
                    connectionAttempt->sender->guid.ToString( addr2 );
                    std::string log = RakNet::format( "Sending ID_NAT_CONNECT_AT_TIME to sender systemAddress %s guid %s", addr1, addr2 );
                    natPunchthroughServerDebugInterface->OnServerMessage( log.c_str() );
                }


                // Same for sender
                bsOut.Reset();
                bsOut.Write( (MessageID)ID_TIMESTAMP );
                bsOut.Write( simultaneousAttemptTime );
                bsOut.Write( (MessageID)ID_NAT_CONNECT_AT_TIME );
                bsOut.Write( connectionAttempt->sessionId );
                bsOut.Write( recipientTargetAddress );                // Public IP, using most recent port
                for( int j = 0; j < MAXIMUM_NUMBER_OF_INTERNAL_IDS; j++ ) // Internal IP
                    bsOut.Write( rakPeerInterface->GetInternalID( recipientSystemAddress, j ) );
                bsOut.Write( connectionAttempt->recipient->guid );
                bsOut.Write( true );
                rakPeerInterface->Send( &bsOut, HIGH_PRIORITY, RELIABLE_ORDERED, 0, senderSystemAddress, false );

                connectionAttempt->recipient->DerefConnectionAttempt( connectionAttempt );
                connectionAttempt->sender->DeleteConnectionAttempt( connectionAttempt );

                // 04/29/08 missing return
                return;
            }
        }
    }
    else
    {

        if( natPunchthroughServerDebugInterface )
        {
            char addr1[128], addr2[128];
            packet->systemAddress.ToString( true, addr1 );
            packet->guid.ToString( addr2 );
            std::string log = RakNet::format( "Ignoring ID_NAT_GET_MOST_RECENT_PORT from systemAddress %s guid %s", addr1, addr2 );
            natPunchthroughServerDebugInterface->OnServerMessage( log.c_str() );
        }
    }
}

void NatPunchthroughServer::StartPunchthroughForUser( User* user )
{
    if( user->isReady == false )
        return;

    User *sender, *recipient, *otherUser;
    for( ConnectionAttempt* connectionAttempt : user->connectionAttempts )
    {
        if( connectionAttempt->sender == user )
        {
            otherUser = connectionAttempt->recipient;
            sender = user;
            recipient = otherUser;
        }
        else
        {
            otherUser = connectionAttempt->sender;
            recipient = user;
            sender = otherUser;
        }

        if( otherUser->isReady )
        {
            if( natPunchthroughServerDebugInterface )
            {
                char str[1024];
                char addr1[128], addr2[128];
                sender->systemAddress.ToString( true, addr1 );
                recipient->systemAddress.ToString( true, addr2 );
                sprintf( str, "Sending NAT_ATTEMPT_PHASE_GETTING_RECENT_PORTS to sender %s and recipient %s.", addr1, addr2 );
                natPunchthroughServerDebugInterface->OnServerMessage( str );
            }

            sender->isReady = false;
            recipient->isReady = false;
            connectionAttempt->attemptPhase = ConnectionAttempt::NAT_ATTEMPT_PHASE_GETTING_RECENT_PORTS;
            connectionAttempt->startTime = RakNet::GetTime();

            sender->mostRecentPort = 0;
            recipient->mostRecentPort = 0;

            BitStream outgoingBs;
            outgoingBs.Write( (MessageID)ID_NAT_GET_MOST_RECENT_PORT );
            // 4/29/09 Write sessionID so we don't use returned port for a system we don't want
            outgoingBs.Write( connectionAttempt->sessionId );
            rakPeerInterface->Send( &outgoingBs, HIGH_PRIORITY, RELIABLE_ORDERED, 0, sender->systemAddress, false );
            rakPeerInterface->Send( &outgoingBs, HIGH_PRIORITY, RELIABLE_ORDERED, 0, recipient->systemAddress, false );

            // 4/22/09 - BUG: missing break statement here
            break;
        }
    }
}

} // namespace RakNet

#endif // _RAKNET_SUPPORT_*
