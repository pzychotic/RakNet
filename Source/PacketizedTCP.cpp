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
#if _RAKNET_SUPPORT_PacketizedTCP == 1 && _RAKNET_SUPPORT_TCPInterface == 1

#include "PacketizedTCP.h"
#include "BitStream.h"
#include "MessageIdentifiers.h"

#include <stdint.h>
#include <string.h>

namespace RakNet {

typedef uint32_t PTCPHeader;

namespace {

// Reads the header at the front of \a bytes without consuming it. False if fewer bytes than
// a header are buffered.
bool PeekHeader( const TCPByteBuffer& bytes, PTCPHeader& dataLength )
{
    if( bytes.Peek( (char*)&dataLength, sizeof( PTCPHeader ) ) == false )
        return false;
    if( BitStream::DoEndianSwap() )
        BitStream::ReverseBytesInPlace( (unsigned char*)&dataLength, sizeof( dataLength ) );
    return true;
}

} // namespace

STATIC_FACTORY_DEFINITIONS( PacketizedTCP, PacketizedTCP );

PacketizedTCP::PacketizedTCP()
{
    maxMessageLength = MAXIMUM_MESSAGE_SIZE;
    messageLengthCapCloseCount = 0;
}
PacketizedTCP::~PacketizedTCP()
{
    ClearAllConnections();
}

void PacketizedTCP::Stop( void )
{
    TCPInterface::Stop();
    for( Packet* pPacket : waitingPackets )
        DeallocatePacket( pPacket );
    waitingPackets.clear();
    ClearAllConnections();
}

void PacketizedTCP::SetMaxMessageLength( unsigned int maxLength )
{
    maxMessageLength = maxLength;
}
unsigned int PacketizedTCP::GetMaxMessageLength( void ) const
{
    return maxMessageLength;
}
uint64_t PacketizedTCP::GetMessageLengthCapCloseCount( void ) const
{
    return messageLengthCapCloseCount;
}

void PacketizedTCP::Send( const char* data, unsigned length, const SystemAddress& systemAddress, bool broadcast )
{
    PTCPHeader dataLength;
    dataLength = length;
#ifndef __BITSTREAM_NATIVE_END
    if( BitStream::DoEndianSwap() )
        BitStream::ReverseBytes( (unsigned char*)&length, (unsigned char*)&dataLength, sizeof( dataLength ) );
#else
    dataLength = length;
#endif

    unsigned int lengthsArray[2];
    const char* dataArray[2];
    dataArray[0] = (char*)&dataLength;
    dataArray[1] = data;
    lengthsArray[0] = sizeof( dataLength );
    lengthsArray[1] = length;
    TCPInterface::SendList( dataArray, lengthsArray, 2, systemAddress, broadcast );
}
bool PacketizedTCP::SendList( const char** data, const unsigned int* lengths, const int numParameters, const SystemAddress& systemAddress, bool broadcast )
{
    if( isStarted == 0 )
        return false;
    if( data == 0 )
        return false;
    if( systemAddress == UNASSIGNED_SYSTEM_ADDRESS && broadcast == false )
        return false;
    PTCPHeader totalLengthOfUserData = 0;
    int i;
    for( i = 0; i < numParameters; i++ )
    {
        if( lengths[i] > 0 )
            totalLengthOfUserData += lengths[i];
    }
    if( totalLengthOfUserData == 0 )
        return false;

    PTCPHeader dataLength;
#ifndef __BITSTREAM_NATIVE_END
    if( BitStream::DoEndianSwap() )
        BitStream::ReverseBytes( (unsigned char*)&totalLengthOfUserData, (unsigned char*)&dataLength, sizeof( dataLength ) );
#else
    dataLength = totalLengthOfUserData;
#endif


    unsigned int lengthsArray[513];
    const char* dataArray[513];
    dataArray[0] = (char*)&dataLength;
    lengthsArray[0] = sizeof( dataLength );
    for( int i = 0; i < 512 && i < numParameters; i++ )
    {
        dataArray[i + 1] = data[i];
        lengthsArray[i + 1] = lengths[i];
    }
    return TCPInterface::SendList( dataArray, lengthsArray, numParameters + 1, systemAddress, broadcast );
}
void PacketizedTCP::PushNotificationsToQueues( void )
{
    // All of them, not one of each: a connection whose new event still waits here has no
    // entry, and what it sends before the event is taken is dropped.
    SystemAddress sa;
    while( ( sa = TCPInterface::HasNewIncomingConnection() ) != UNASSIGNED_SYSTEM_ADDRESS )
    {
        _newIncomingConnections.push_back( sa );
        CountConnection( sa, 1 );
    }

    while( ( sa = TCPInterface::HasFailedConnectionAttempt() ) != UNASSIGNED_SYSTEM_ADDRESS )
    {
        _failedConnectionAttempts.push_back( sa );
    }

    while( ( sa = TCPInterface::HasLostConnection() ) != UNASSIGNED_SYSTEM_ADDRESS )
    {
        _lostConnections.push_back( sa );
        CountConnection( sa, -1 );
    }

    while( ( sa = TCPInterface::HasCompletedConnectionAttempt() ) != UNASSIGNED_SYSTEM_ADDRESS )
    {
        _completedConnectionAttempts.push_back( sa );
        CountConnection( sa, 1 );
    }
}
Packet* PacketizedTCP::Receive( void )
{
    PushNotificationsToQueues();

    for( PluginInterface2* pPlugin : messageHandlerList )
    {
        pPlugin->Update();
    }

    Packet* outgoingPacket = ReturnOutgoingPacket();
    if( outgoingPacket )
        return outgoingPacket;

    // One read at a time, and only while nothing waits: what one read frames is delivered
    // before the next is taken. So a client whose messages the application has not taken
    // stays at TCPInterface's incoming cap, and zero-length frames make at most one read's
    // worth of Packets.
    Packet* incomingPacket;
    while( waitingPackets.empty() && ( incomingPacket = TCPInterface::ReceiveInt() ) != 0 )
    {
        const auto it = connections.find( incomingPacket->systemAddress );
        if( it == connections.end() )
        {
            DeallocatePacket( incomingPacket );
            continue;
        }

        if( incomingPacket->deleteData == true )
        {
            // Came from network. Nothing from a connection closed at the message length cap
            // is framed: the stream lost its framing there.
            Connection* connection = it->second;
            if( connection->openConnections > 0 && connection->isClosed == false )
                FrameMessages( *incomingPacket, *connection );
            DeallocatePacket( incomingPacket );
        }
        else
        {
            waitingPackets.push_back( incomingPacket );
        }
    }

    return ReturnOutgoingPacket();
}
void PacketizedTCP::FrameMessages( const Packet& incomingPacket, Connection& connection )
{
    TCPByteBuffer& bytes = connection.bytes;
    // Buffer data
    bytes.Append( (const char*)incomingPacket.data, incomingPacket.length );
    const SystemAddress systemAddressFromPacket = incomingPacket.systemAddress;

    // Header indicates packet length. Read out every message that is complete, checking
    // each header against the maximum before its message is buffered any further.
    PTCPHeader dataLength;
    bool isAnyFramed = false;
    while( PeekHeader( bytes, dataLength ) )
    {
        if( dataLength > maxMessageLength )
        {
            CloseOverlongSender( systemAddressFromPacket, connection );
            return;
        }

        if( bytes.Size() < (uint64_t)dataLength + sizeof( PTCPHeader ) )
            break;

        bytes.Consume( sizeof( PTCPHeader ) );
        Packet* outgoingPacket = RakNet::OP_NEW<Packet>( _FILE_AND_LINE_ );
        outgoingPacket->length = dataLength;
        outgoingPacket->bitSize = BYTES_TO_BITS( dataLength );
        outgoingPacket->guid = UNASSIGNED_RAKNET_GUID;
        outgoingPacket->systemAddress = systemAddressFromPacket;
        outgoingPacket->deleteData = false; // Did not come from the network
        outgoingPacket->data = (unsigned char*)rakMalloc_Ex( dataLength, _FILE_AND_LINE_ );
        if( outgoingPacket->data == 0 )
        {
            notifyOutOfMemory( _FILE_AND_LINE_ );
            RakNet::OP_DELETE( outgoingPacket, _FILE_AND_LINE_ );
            return;
        }
        bytes.Read( (char*)outgoingPacket->data, dataLength );

        waitingPackets.push_back( outgoingPacket );
        isAnyFramed = true;
    }

    // A message still arriving, with nothing framed out of this read
    if( isAnyFramed || bytes.Size() < sizeof( PTCPHeader ) )
        return;

    // At most a header and maxMessageLength bytes, so it fits an unsigned int.
    unsigned int newWritten = (unsigned int)bytes.Size();
    unsigned int oldWritten = newWritten - incomingPacket.length;

    // Return ID_DOWNLOAD_PROGRESS
    if( newWritten / 65536 != oldWritten / 65536 )
    {
        // The chunk is the message's first 64 KiB, or as much of it as follows the header
        // so far.
        const unsigned int bufferedAfterHeader = newWritten - sizeof( PTCPHeader );
        const unsigned int oneChunkSize = bufferedAfterHeader < 65536 ? bufferedAfterHeader : 65536;

        Packet* outgoingPacket = RakNet::OP_NEW<Packet>( _FILE_AND_LINE_ );
        outgoingPacket->length = sizeof( MessageID ) +
                                    sizeof( unsigned int ) * 2 +
                                    sizeof( unsigned int ) +
                                    oneChunkSize;
        outgoingPacket->bitSize = BYTES_TO_BITS( outgoingPacket->length );
        outgoingPacket->guid = UNASSIGNED_RAKNET_GUID;
        outgoingPacket->systemAddress = incomingPacket.systemAddress;
        outgoingPacket->deleteData = false;
        outgoingPacket->data = (unsigned char*)rakMalloc_Ex( outgoingPacket->length, _FILE_AND_LINE_ );
        if( outgoingPacket->data == 0 )
        {
            notifyOutOfMemory( _FILE_AND_LINE_ );
            RakNet::OP_DELETE( outgoingPacket, _FILE_AND_LINE_ );
            return;
        }

        outgoingPacket->data[0] = (MessageID)ID_DOWNLOAD_PROGRESS;
        unsigned int totalParts = dataLength / 65536;
        unsigned int partIndex = newWritten / 65536;
        memcpy( outgoingPacket->data + sizeof( MessageID ), &partIndex, sizeof( unsigned int ) );
        memcpy( outgoingPacket->data + sizeof( MessageID ) + sizeof( unsigned int ) * 1, &totalParts, sizeof( unsigned int ) );
        memcpy( outgoingPacket->data + sizeof( MessageID ) + sizeof( unsigned int ) * 2, &oneChunkSize, sizeof( unsigned int ) );
        size_t bufferedLength;
        const char* buffered = bytes.Contiguous( &bufferedLength );
        memcpy( outgoingPacket->data + sizeof( MessageID ) + sizeof( unsigned int ) * 3, buffered + sizeof( PTCPHeader ), oneChunkSize );

        waitingPackets.push_back( outgoingPacket );
    }
}
void PacketizedTCP::CloseOverlongSender( const SystemAddress& sa, Connection& connection )
{
    // Once per interface: a client repeating it should not flood the console too.
    if( messageLengthCapCloseCount++ == 0 )
    {
        RAKNET_DEBUG_PRINTF( "PacketizedTCP: closing a connection that announced a message longer than %u bytes (SetMaxMessageLength). See GetMessageLengthCapCloseCount.\n", maxMessageLength );
    }

    // What it sent is discarded, and so is anything still queued from it.
    connection.bytes.Clear();
    connection.isClosed = true;

    // Reported like any lost connection. If TCPInterface found it already lost, its lost
    // event is on the way and does both of these instead.
    if( TCPInterface::CloseConnection( sa, LCR_CONNECTION_LOST ) )
    {
        _lostConnections.push_back( sa );
        CountConnection( sa, -1 );
    }
}
Packet* PacketizedTCP::ReturnOutgoingPacket( void )
{
    Packet* outgoingPacket = 0;
    while( outgoingPacket == 0 && !waitingPackets.empty() )
    {
        outgoingPacket = waitingPackets.front();
        waitingPackets.pop_front();

        for( PluginInterface2* pPlugin : messageHandlerList )
        {
            PluginReceiveResult pluginResult = pPlugin->OnReceive( outgoingPacket );
            if( pluginResult == RR_STOP_PROCESSING_AND_DEALLOCATE )
            {
                DeallocatePacket( outgoingPacket );
                outgoingPacket = 0; // Will do the loop again and get another packet
                break;              // break out of the enclosing for
            }
            else if( pluginResult == RR_STOP_PROCESSING )
            {
                outgoingPacket = 0;
                break;
            }
        }
    }

    return outgoingPacket;
}
bool PacketizedTCP::CloseConnection( SystemAddress systemAddress )
{
    // A connection TCPInterface found already lost keeps its entry until its lost event.
    if( TCPInterface::CloseConnection( systemAddress ) == false )
        return false;
    CountConnection( systemAddress, -1 );
    return true;
}

void PacketizedTCP::CountConnection( const SystemAddress& sa, int delta )
{
    if( sa == UNASSIGNED_SYSTEM_ADDRESS )
        return;

    // One entry per address, so a reconnect from the same address shares the entry of the
    // connection it replaces rather than failing to insert its own.
    auto it = connections.find( sa );
    if( it == connections.end() )
        it = connections.insert( std::make_pair( sa, RakNet::OP_NEW<Connection>( _FILE_AND_LINE_ ) ) ).first;

    Connection* connection = it->second;
    if( delta > 0 )
    {
        // A new stream: whatever the old connection left unframed is not part of it.
        connection->bytes.Clear();
        connection->isClosed = false;
    }

    connection->openConnections += delta;
    if( connection->openConnections == 0 )
    {
        RakNet::OP_DELETE( connection, _FILE_AND_LINE_ );
        connections.erase( it );
    }
}

void PacketizedTCP::ClearAllConnections( void )
{
    for( const auto& kEntry : connections )
    {
        RakNet::OP_DELETE( kEntry.second, _FILE_AND_LINE_ );
    }
    connections.clear();
}

SystemAddress PacketizedTCP::HasCompletedConnectionAttempt( void )
{
    PushNotificationsToQueues();

    if( !_completedConnectionAttempts.empty() )
    {
        SystemAddress sa = _completedConnectionAttempts.front();
        _completedConnectionAttempts.pop_front();
        return sa;
    }
    return UNASSIGNED_SYSTEM_ADDRESS;
}
SystemAddress PacketizedTCP::HasFailedConnectionAttempt( void )
{
    PushNotificationsToQueues();

    if( !_failedConnectionAttempts.empty() )
    {
        SystemAddress sa = _failedConnectionAttempts.front();
        _failedConnectionAttempts.pop_front();
        return sa;
    }
    return UNASSIGNED_SYSTEM_ADDRESS;
}
SystemAddress PacketizedTCP::HasNewIncomingConnection( void )
{
    PushNotificationsToQueues();

    if( !_newIncomingConnections.empty() )
    {
        SystemAddress sa = _newIncomingConnections.front();
        _newIncomingConnections.pop_front();
        return sa;
    }
    return UNASSIGNED_SYSTEM_ADDRESS;
}
SystemAddress PacketizedTCP::HasLostConnection( void )
{
    PushNotificationsToQueues();

    if( !_lostConnections.empty() )
    {
        SystemAddress sa = _lostConnections.front();
        _lostConnections.pop_front();
        return sa;
    }
    return UNASSIGNED_SYSTEM_ADDRESS;
}

} // namespace RakNet

#endif // _RAKNET_SUPPORT_*
