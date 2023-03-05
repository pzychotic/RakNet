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
#if _RAKNET_SUPPORT_MessageFilter == 1

#include "Plugins/MessageFilter.h"
#include "RakAssert.h"
#include "GetTime.h"
#include "MessageIdentifiers.h"
#include "RakPeerInterface.h"
#include "TCPInterface.h"
#include "BitStream.h"

namespace RakNet {

int MessageFilterStrComp( char* const& key, char* const& data )
{
    return strcmp( key, data );
}

int FilterSetComp( const int& key, FilterSet* const& data )
{
    if( key < data->filterSetID )
        return -1;
    else if( key == data->filterSetID )
        return 0;
    else
        return 1;
}
STATIC_FACTORY_DEFINITIONS( MessageFilter, MessageFilter );

MessageFilter::MessageFilter()
{
    whenLastTimeoutCheck = RakNet::GetTime();
}
MessageFilter::~MessageFilter()
{
    Clear();
}
void MessageFilter::SetAutoAddNewConnectionsToFilter( int filterSetID )
{
    autoAddNewConnectionsToFilter = filterSetID;
}
void MessageFilter::SetAllowMessageID( bool allow, int messageIDStart, int messageIDEnd, int filterSetID )
{
    RakAssert( messageIDStart <= messageIDEnd );
    FilterSet* filterSet = GetFilterSetByID( filterSetID );
    int i;
    for( i = messageIDStart; i <= messageIDEnd; ++i )
        filterSet->allowedIDs[i] = allow;
}
void MessageFilter::SetAllowRPC4( bool allow, const char* uniqueID, int filterSetID )
{
    FilterSet* filterSet = GetFilterSetByID( filterSetID );
    bool objectExists;
    unsigned int idx = filterSet->allowedRPC4.GetIndexFromKey( uniqueID, &objectExists );
    if( allow )
    {
        if( objectExists == false )
        {
            filterSet->allowedRPC4.InsertAtIndex( uniqueID, idx, _FILE_AND_LINE_ );
            filterSet->allowedIDs[ID_RPC_PLUGIN] = true;
        }
    }
    else
    {
        if( objectExists == true )
        {
            filterSet->allowedRPC4.RemoveAtIndex( idx );
            if( filterSet->allowedRPC4.Size() == 0 )
            {
                filterSet->allowedIDs[ID_RPC_PLUGIN] = false;
            }
        }
    }
}
void MessageFilter::SetActionOnDisallowedMessage( bool kickOnDisallowed, bool banOnDisallowed, RakNet::TimeMS banTimeMS, int filterSetID )
{
    FilterSet* filterSet = GetFilterSetByID( filterSetID );
    filterSet->kickOnDisallowedMessage = kickOnDisallowed;
    filterSet->disallowedMessageBanTimeMS = banTimeMS;
    filterSet->banOnDisallowedMessage = banOnDisallowed;
}
void MessageFilter::SetDisallowedMessageCallback( int filterSetID, void* userData, void ( *invalidMessageCallback )( RakPeerInterface* peer, AddressOrGUID systemAddress, int filterSetID, void* userData, unsigned char messageID ) )
{
    FilterSet* filterSet = GetFilterSetByID( filterSetID );
    filterSet->invalidMessageCallback = invalidMessageCallback;
    filterSet->disallowedCallbackUserData = userData;
}
void MessageFilter::SetTimeoutCallback( int filterSetID, void* userData, void ( *invalidMessageCallback )( RakPeerInterface* peer, AddressOrGUID systemAddress, int filterSetID, void* userData ) )
{
    FilterSet* filterSet = GetFilterSetByID( filterSetID );
    filterSet->timeoutCallback = invalidMessageCallback;
    filterSet->timeoutUserData = userData;
}
void MessageFilter::SetFilterMaxTime( int allowedTimeMS, bool banOnExceed, RakNet::TimeMS banTimeMS, int filterSetID )
{
    FilterSet* filterSet = GetFilterSetByID( filterSetID );
    filterSet->maxMemberTimeMS = allowedTimeMS;
    filterSet->banOnFilterTimeExceed = banOnExceed;
    filterSet->timeExceedBanTimeMS = banTimeMS;
}

int MessageFilter::GetSystemFilterSet( AddressOrGUID systemAddress )
{
    if( auto it = systemList.find( systemAddress ); it != systemList.end() )
    {
        return it->second.filter->filterSetID;
    }
    return -1;
}

void MessageFilter::SetSystemFilterSet( AddressOrGUID addressOrGUID, int filterSetID )
{
    // Allocate this filter set if it doesn't exist.
    RakAssert( addressOrGUID.IsUndefined() == false );

    auto it = systemList.find( addressOrGUID );
    if( it == systemList.end() )
    {
        if( filterSetID < 0 )
            return;

        systemList.insert( std::make_pair( addressOrGUID, FilteredSystem{ GetFilterSetByID( filterSetID ), RakNet::GetTimeMS() } ) );
    }
    else
    {
        if( filterSetID >= 0 )
        {
            it->second.filter = GetFilterSetByID( filterSetID );
            it->second.timeEnteredThisSet = RakNet::GetTimeMS();
        }
        else
        {
            systemList.erase( it );
        }
    }
}

unsigned MessageFilter::GetSystemCount( int filterSetID ) const
{
    if( filterSetID == -1 )
    {
        return static_cast<unsigned>( systemList.size() );
    }
    else
    {
        unsigned int count = 0u;
        for( auto&& [key, value] : systemList )
        {
            if( value.filter->filterSetID == filterSetID )
            {
                ++count;
            }
        }
        return count;
    }
}

unsigned MessageFilter::GetFilterSetCount( void ) const
{
    return filterList.Size();
}
int MessageFilter::GetFilterSetIDByIndex( unsigned index )
{
    return filterList[index]->filterSetID;
}

void MessageFilter::DeleteFilterSet( int filterSetID )
{
    bool objectExists = false;
    unsigned int index = filterList.GetIndexFromKey( filterSetID, &objectExists );
    if( objectExists )
    {
        FilterSet* filterSet = filterList[index];
        DeallocateFilterSet( filterSet );
        filterList.RemoveAtIndex( index );

        for( auto it = systemList.begin(); it != systemList.end(); )
        {
            if( it->second.filter == filterSet )
            {
                it = systemList.erase( it );
            }
            else
            {
                ++it;
            }
        }
    }
}

void MessageFilter::Clear( void )
{
    systemList.clear();
    for( unsigned int i = 0; i < filterList.Size(); i++ )
        DeallocateFilterSet( filterList[i] );
    filterList.Clear( false, _FILE_AND_LINE_ );
}

void MessageFilter::DeallocateFilterSet( FilterSet* filterSet )
{
    RakNet::OP_DELETE( filterSet, _FILE_AND_LINE_ );
}

FilterSet* MessageFilter::GetFilterSetByID( int filterSetID )
{
    RakAssert( filterSetID >= 0 );
    bool objectExists = false;
    unsigned int index = filterList.GetIndexFromKey( filterSetID, &objectExists );
    if( objectExists )
        return filterList[index];
    else
    {
        FilterSet* newFilterSet = RakNet::OP_NEW<FilterSet>( _FILE_AND_LINE_ );
        memset( newFilterSet->allowedIDs, 0, MESSAGE_FILTER_MAX_MESSAGE_ID * sizeof( bool ) );
        newFilterSet->banOnFilterTimeExceed = false;
        newFilterSet->kickOnDisallowedMessage = false;
        newFilterSet->banOnDisallowedMessage = false;
        newFilterSet->disallowedMessageBanTimeMS = 0;
        newFilterSet->timeExceedBanTimeMS = 0;
        newFilterSet->maxMemberTimeMS = 0;
        newFilterSet->filterSetID = filterSetID;
        newFilterSet->invalidMessageCallback = 0;
        newFilterSet->timeoutCallback = 0;
        newFilterSet->timeoutUserData = 0;
        filterList.Insert( filterSetID, newFilterSet, true, _FILE_AND_LINE_ );
        return newFilterSet;
    }
}

void MessageFilter::OnInvalidMessage( FilterSet* filterSet, AddressOrGUID systemAddress, unsigned char messageID )
{
    if( filterSet->invalidMessageCallback )
        filterSet->invalidMessageCallback( rakPeerInterface, systemAddress, filterSet->filterSetID, filterSet->disallowedCallbackUserData, messageID );
    if( filterSet->banOnDisallowedMessage && rakPeerInterface )
    {
        char str1[64];
        systemAddress.systemAddress.ToString( false, str1 );
        rakPeerInterface->AddToBanList( str1, filterSet->disallowedMessageBanTimeMS );
    }
    if( filterSet->kickOnDisallowedMessage )
    {
        if( rakPeerInterface )
            rakPeerInterface->CloseConnection( systemAddress, true, 0 );
#if _RAKNET_SUPPORT_PacketizedTCP == 1 && _RAKNET_SUPPORT_TCPInterface == 1
        else
            tcpInterface->CloseConnection( systemAddress.systemAddress );
#endif
    }
}

void MessageFilter::Update( void )
{
    // Update all timers for all systems.  If those systems' filter sets are expired, take the appropriate action.
    RakNet::Time curTime = RakNet::GetTime();
    if( GreaterThan( curTime - 1000, whenLastTimeoutCheck ) )
    {
        for( auto it = systemList.begin(); it != systemList.end(); )
        {
            const AddressOrGUID& key = it->first;
            const FilteredSystem& value = it->second;
            const FilterSet* filter = value.filter;

            if( filter && filter->maxMemberTimeMS > 0 &&
                curTime - value.timeEnteredThisSet >= filter->maxMemberTimeMS )
            {
                if( filter->timeoutCallback )
                {
                    filter->timeoutCallback( rakPeerInterface, key, filter->filterSetID, filter->timeoutUserData );
                }

                if( filter->banOnFilterTimeExceed && rakPeerInterface )
                {
                    char str1[64];
                    key.ToString( false, str1 );
                    rakPeerInterface->AddToBanList( str1, filter->timeExceedBanTimeMS );
                }
                if( rakPeerInterface )
                    rakPeerInterface->CloseConnection( key, true, 0 );
#if _RAKNET_SUPPORT_PacketizedTCP == 1 && _RAKNET_SUPPORT_TCPInterface == 1
                else
                    tcpInterface->CloseConnection( key.systemAddress );
#endif

                it = systemList.erase( it );
            }
        }

        whenLastTimeoutCheck = curTime + 1000;
    }
}

void MessageFilter::OnNewConnection( const SystemAddress& systemAddress, RakNetGUID rakNetGUID, bool isIncoming )
{
    (void)isIncoming;

    AddressOrGUID aog;
    aog.rakNetGuid = rakNetGUID;
    aog.systemAddress = systemAddress;

    // New system, automatically assign to filter set if appropriate
    if( autoAddNewConnectionsToFilter >= 0 && systemList.find( aog ) == systemList.end() )
    {
        SetSystemFilterSet( aog, autoAddNewConnectionsToFilter );
    }
}

void MessageFilter::OnClosedConnection( const SystemAddress& systemAddress, RakNetGUID rakNetGUID, PI2_LostConnectionReason lostConnectionReason )
{
    (void)lostConnectionReason;

    AddressOrGUID aog;
    aog.rakNetGuid = rakNetGUID;
    aog.systemAddress = systemAddress;

    // Lost system, remove from the list
    systemList.erase( aog );
}

PluginReceiveResult MessageFilter::OnReceive( Packet* packet )
{
    unsigned char messageId = packet->data[0];

    switch( messageId )
    {
    case ID_NEW_INCOMING_CONNECTION:
    case ID_CONNECTION_REQUEST_ACCEPTED:
    case ID_CONNECTION_LOST:
    case ID_DISCONNECTION_NOTIFICATION:
    case ID_CONNECTION_ATTEMPT_FAILED:
    case ID_NO_FREE_INCOMING_CONNECTIONS:
    case ID_IP_RECENTLY_CONNECTED:
    case ID_CONNECTION_BANNED:
    case ID_INVALID_PASSWORD:
    case ID_UNCONNECTED_PONG:
    case ID_ALREADY_CONNECTED:
    case ID_ADVERTISE_SYSTEM:
    case ID_REMOTE_DISCONNECTION_NOTIFICATION:
    case ID_REMOTE_CONNECTION_LOST:
    case ID_REMOTE_NEW_INCOMING_CONNECTION:
    case ID_DOWNLOAD_PROGRESS:
        break;
    default:
        if( packet->data[0] == ID_TIMESTAMP )
        {
            if( packet->length < sizeof( MessageID ) + sizeof( RakNet::TimeMS ) )
                return RR_STOP_PROCESSING_AND_DEALLOCATE; // Invalid message
            messageId = packet->data[sizeof( MessageID ) + sizeof( RakNet::TimeMS )];
        }
        // If this system is filtered, check if this message is allowed.  If not allowed, return RR_STOP_PROCESSING_AND_DEALLOCATE
        auto it = systemList.find( packet );
        if( it == systemList.end() )
            break;

        const FilteredSystem& value = it->second;

        if( value.filter->allowedIDs[messageId] == false )
        {
            OnInvalidMessage( value.filter, packet, packet->data[0] );
            return RR_STOP_PROCESSING_AND_DEALLOCATE;
        }

        if( packet->data[0] == ID_RPC_PLUGIN )
        {
            BitStream bsIn( packet->data, packet->length, false );
            bsIn.IgnoreBytes( 2 );
            std::string functionName;
            bsIn.ReadCompressed( *functionName.data() );
            if( value.filter->allowedRPC4.HasData( functionName ) == false )
            {
                OnInvalidMessage( value.filter, packet, packet->data[0] );
                return RR_STOP_PROCESSING_AND_DEALLOCATE;
            }
        }

        break;
    }

    return RR_CONTINUE_PROCESSING;
}

} // namespace RakNet

#endif // _RAKNET_SUPPORT_*
