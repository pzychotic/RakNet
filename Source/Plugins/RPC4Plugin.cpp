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
#if _RAKNET_SUPPORT_RPC4Plugin == 1

#include "Plugins/RPC4Plugin.h"
#include "MessageIdentifiers.h"
#include "RakPeerInterface.h"
#include "TCPInterface.h"
#include "RakNetDefines.h"
//#include "GetTime.h"

#include <chrono>
#include <deque>
#include <memory>
#include <string.h>
#include <thread>
#include <tuple>
#include <vector>

namespace RakNet {

STATIC_FACTORY_DEFINITIONS( RPC4, RPC4 );

struct GlobalRegistration
{
    void ( *registerFunctionPointer )( BitStream* userData, Packet* packet );
    void ( *registerBlockingFunctionPointer )( BitStream* userData, BitStream* returnData, Packet* packet );
    std::string functionName;
    MessageID messageId;
    int callPriority;
};

// Built on first use, so a registration from another translation unit during static
// initialisation finds it constructed whatever the initialisation order.
static std::vector<GlobalRegistration>& GlobalRegistrations()
{
    static std::vector<GlobalRegistration> registrations;
    return registrations;
}

RPC4GlobalRegistration::RPC4GlobalRegistration( const char* uniqueID, void ( *functionPointer )( BitStream* userData, Packet* packet ) )
{
    GlobalRegistrations().push_back( { functionPointer, nullptr, uniqueID, 0, (int)0xFFFFFFFF } );
}
RPC4GlobalRegistration::RPC4GlobalRegistration( const char* uniqueID, void ( *functionPointer )( BitStream* userData, Packet* packet ), int callPriority )
{
    RakAssert( callPriority != (int)0xFFFFFFFF );
    GlobalRegistrations().push_back( { functionPointer, nullptr, uniqueID, 0, callPriority } );
}
RPC4GlobalRegistration::RPC4GlobalRegistration( const char* uniqueID, void ( *functionPointer )( BitStream* userData, BitStream* returnData, Packet* packet ) )
{
    GlobalRegistrations().push_back( { nullptr, functionPointer, uniqueID, 0, 0 } );
}
RPC4GlobalRegistration::RPC4GlobalRegistration( const char* uniqueID, MessageID messageId )
{
    GlobalRegistrations().push_back( { nullptr, nullptr, uniqueID, messageId, 0 } );
}

enum RPC4Identifiers
{
    ID_RPC4_CALL,
    ID_RPC4_RETURN,
    ID_RPC4_SIGNAL,
};

RPC4::RPC4()
{
    gotBlockingReturnValue = false;
    nextSlotRegistrationCount = 0;
    interruptSignal = false;
}

RPC4::~RPC4() = default;

bool RPC4::RegisterFunction( const char* uniqueID, void ( *functionPointer )( BitStream* userData, Packet* packet ) )
{
    return registeredNonblockingFunctions.try_emplace( uniqueID, functionPointer ).second;
}

void RPC4::RegisterSlot( const char* sharedIdentifier, void ( *functionPointer )( BitStream* userData, Packet* packet ), int callPriority )
{
    const auto [it, inserted] = localSlots.try_emplace( sharedIdentifier );
    if( inserted )
        it->second = std::make_unique<LocalSlot>();

    LocalSlotObject lso( nextSlotRegistrationCount++, callPriority, functionPointer );
    it->second->slotObjects.insert( lso );
}

bool RPC4::RegisterBlockingFunction( const char* uniqueID, void ( *functionPointer )( BitStream* userData, BitStream* returnData, Packet* packet ) )
{
    return registeredBlockingFunctions.try_emplace( uniqueID, functionPointer ).second;
}

void RPC4::RegisterLocalCallback( const char* uniqueID, MessageID messageId )
{
    std::unique_ptr<LocalCallback>& lc = localCallbacks[messageId];
    if( lc == nullptr )
    {
        lc = std::make_unique<LocalCallback>();
        lc->messageId = messageId;
    }
    lc->functions.insert( uniqueID );
}

bool RPC4::UnregisterFunction( const char* uniqueID )
{
    return registeredNonblockingFunctions.erase( uniqueID ) > 0u;
}

bool RPC4::UnregisterBlockingFunction( const char* uniqueID )
{
    return registeredBlockingFunctions.erase( uniqueID ) > 0u;
}

bool RPC4::UnregisterLocalCallback( const char* uniqueID, MessageID messageId )
{
    const auto it = localCallbacks.find( messageId );
    if( it != localCallbacks.end() )
    {
        LocalCallback* lc = it->second.get();
        if( lc->functions.erase( uniqueID ) != 0 && lc->functions.empty() )
        {
            localCallbacks.erase( it );
            return true;
        }
    }
    return false;
}

bool RPC4::UnregisterSlot( const char* sharedIdentifier )
{
    if( auto it = localSlots.find( sharedIdentifier ); it != localSlots.end() )
    {
        localSlots.erase( it );
        return true;
    }

    return false;
}

void RPC4::CallLoopback( const char* uniqueID, BitStream* bitStream )
{
    Packet* p = 0;

    if( registeredNonblockingFunctions.find( uniqueID ) == registeredNonblockingFunctions.end() )
    {
        // Same framing as the error OnReceive sends over the network: the two
        // identifier bytes, then the function name length-prefixed by Write. The
        // only reader of either - the drain loop in CallBlocking, which this
        // packet passes through - parses that form, so building it by hand with
        // strcpy left it unparseable.
        BitStream bsOut;
        bsOut.Write( (MessageID)ID_RPC_REMOTE_ERROR );
        bsOut.Write( (unsigned char)RPC_ERROR_FUNCTION_NOT_REGISTERED );
        bsOut.Write( std::string( uniqueID ) );

        if( rakPeerInterface )
            p = AllocatePacketUnified( bsOut.GetNumberOfBytesUsed() );
#if _RAKNET_SUPPORT_PacketizedTCP == 1 && _RAKNET_SUPPORT_TCPInterface == 1
        else
            p = tcpInterface->AllocatePacket( bsOut.GetNumberOfBytesUsed() );
#endif
        if( p == 0 )
            return;

        if( rakPeerInterface )
            p->guid = rakPeerInterface->GetGuidFromSystemAddress( UNASSIGNED_SYSTEM_ADDRESS );
#if _RAKNET_SUPPORT_PacketizedTCP == 1 && _RAKNET_SUPPORT_TCPInterface == 1
        else
            p->guid = UNASSIGNED_RAKNET_GUID;
#endif

        p->systemAddress = UNASSIGNED_SYSTEM_ADDRESS;
        p->systemAddress.systemIndex = (SystemIndex)-1;
        memcpy( p->data, bsOut.GetData(), bsOut.GetNumberOfBytesUsed() );

        PushBackPacketUnified( p, false );

        return;
    }

    BitStream out;
    out.Write( (MessageID)ID_RPC_PLUGIN );
    out.Write( (MessageID)ID_RPC4_CALL );
    out.WriteCompressed( std::string( uniqueID ) );
    out.Write( false ); // nonblocking
    if( bitStream )
    {
        bitStream->ResetReadPointer();
        out.AlignWriteToByteBoundary();
        out.Write( bitStream );
    }
    if( rakPeerInterface )
        p = AllocatePacketUnified( out.GetNumberOfBytesUsed() );
#if _RAKNET_SUPPORT_PacketizedTCP == 1 && _RAKNET_SUPPORT_TCPInterface == 1
    else
        p = tcpInterface->AllocatePacket( out.GetNumberOfBytesUsed() );
#endif
    if( p == 0 )
        return;

    if( rakPeerInterface )
        p->guid = rakPeerInterface->GetGuidFromSystemAddress( UNASSIGNED_SYSTEM_ADDRESS );
#if _RAKNET_SUPPORT_PacketizedTCP == 1 && _RAKNET_SUPPORT_TCPInterface == 1
    else
        p->guid = UNASSIGNED_RAKNET_GUID;
#endif
    p->systemAddress = UNASSIGNED_SYSTEM_ADDRESS;
    p->systemAddress.systemIndex = (SystemIndex)-1;
    memcpy( p->data, out.GetData(), out.GetNumberOfBytesUsed() );
    PushBackPacketUnified( p, false );
    return;
}

void RPC4::Call( const char* uniqueID, BitStream* bitStream, PacketPriority priority, PacketReliability reliability, char orderingChannel, const AddressOrGUID systemIdentifier, bool broadcast )
{
    BitStream out;
    out.Write( (MessageID)ID_RPC_PLUGIN );
    out.Write( (MessageID)ID_RPC4_CALL );
    out.WriteCompressed( std::string( uniqueID ) );
    out.Write( false ); // Nonblocking
    if( bitStream )
    {
        bitStream->ResetReadPointer();
        out.AlignWriteToByteBoundary();
        out.Write( bitStream );
    }
    SendUnified( &out, priority, reliability, orderingChannel, systemIdentifier, broadcast );
}

bool RPC4::CallBlocking( const char* uniqueID, BitStream* bitStream, PacketPriority priority, PacketReliability reliability, char orderingChannel, const AddressOrGUID systemIdentifier, BitStream* returnData )
{
    BitStream out;
    out.Write( (MessageID)ID_RPC_PLUGIN );
    out.Write( (MessageID)ID_RPC4_CALL );
    out.WriteCompressed( std::string( uniqueID ) );
    out.Write( true ); // Blocking
    if( bitStream )
    {
        bitStream->ResetReadPointer();
        out.AlignWriteToByteBoundary();
        out.Write( bitStream );
    }
    RakAssert( returnData );
    RakAssert( rakPeerInterface );
    ConnectionState cs;
    cs = rakPeerInterface->GetConnectionState( systemIdentifier );
    if( cs != IS_CONNECTED )
        return false;

    SendUnified( &out, priority, reliability, orderingChannel, systemIdentifier, false );

    returnData->Reset();
    blockingReturnValue.Reset();
    gotBlockingReturnValue = false;
    std::deque<Packet*> packetQueue;
    while( gotBlockingReturnValue == false )
    {
        // TODO - block, filter until gotBlockingReturnValue==true or ID_CONNECTION_LOST or ID_DISCONNECTION_NOTIFICXATION or ID_RPC_REMOTE_ERROR/RPC_ERROR_FUNCTION_NOT_REGISTERED
        std::this_thread::sleep_for( std::chrono::milliseconds( 30 ) );

        Packet* packet = rakPeerInterface->Receive();

        if( packet )
        {
            if(
                ( packet->data[0] == ID_CONNECTION_LOST || packet->data[0] == ID_DISCONNECTION_NOTIFICATION ) &&
                ( ( systemIdentifier.rakNetGuid != UNASSIGNED_RAKNET_GUID && packet->guid == systemIdentifier.rakNetGuid ) ||
                  ( systemIdentifier.systemAddress != UNASSIGNED_SYSTEM_ADDRESS && packet->systemAddress == systemIdentifier.systemAddress ) ) )
            {
                // Push back to head in reverse order
                rakPeerInterface->PushBackPacket( packet, true );
                for( Packet* pPacket : packetQueue )
                {
                    rakPeerInterface->PushBackPacket( pPacket, true );
                }
                packetQueue.clear();
                return false;
            }
            else if( packet->data[0] == ID_RPC_REMOTE_ERROR && packet->data[1] == RPC_ERROR_FUNCTION_NOT_REGISTERED )
            {
                std::string functionName;
                BitStream bsIn( packet->data, packet->length, false );
                bsIn.IgnoreBytes( 2 );
                if( bsIn.Read( functionName ) && functionName == uniqueID )
                {
                    // Push back to head in reverse order
                    rakPeerInterface->PushBackPacket( packet, true );
                    for( Packet* pPacket : packetQueue )
                    {
                        rakPeerInterface->PushBackPacket( pPacket, true );
                    }
                    packetQueue.clear();
                    return false;
                }
                else
                {
                    packetQueue.push_front( packet );
                }
            }
            else
            {
                packetQueue.push_front( packet );
            }
        }
    }

    returnData->Write( blockingReturnValue );
    returnData->ResetReadPointer();
    return true;
}

void RPC4::Signal( const char* sharedIdentifier, BitStream* bitStream, PacketPriority priority, PacketReliability reliability, char orderingChannel, const AddressOrGUID systemIdentifier, bool broadcast, bool invokeLocal )
{
    BitStream out;
    out.Write( (MessageID)ID_RPC_PLUGIN );
    out.Write( (MessageID)ID_RPC4_SIGNAL );
    out.WriteCompressed( std::string( sharedIdentifier ) );
    if( bitStream )
    {
        bitStream->ResetReadPointer();
        out.AlignWriteToByteBoundary();
        out.Write( bitStream );
    }
    SendUnified( &out, priority, reliability, orderingChannel, systemIdentifier, broadcast );

    if( invokeLocal )
    {
        auto it = localSlots.find( sharedIdentifier );

        if( it == localSlots.end() )
            return;

        Packet p;
        p.guid = rakPeerInterface->GetMyGUID();
        p.systemAddress = rakPeerInterface->GetInternalID( UNASSIGNED_SYSTEM_ADDRESS );
        p.wasGeneratedLocally = true;
        BitStream *bsptr, bstemp;
        if( bitStream )
        {
            bitStream->ResetReadPointer();
            p.length = bitStream->GetNumberOfBytesUsed();
            p.bitSize = bitStream->GetNumberOfBitsUsed();
            bsptr = bitStream;
        }
        else
        {
            p.length = 0;
            p.bitSize = 0;
            bsptr = &bstemp;
        }

        InvokeSignal( it->second.get(), bsptr, &p );
    }
}

void RPC4::InvokeSignal( LocalSlot* localSlot, BitStream* serializedParameters, Packet* packet )
{
    interruptSignal = false;
    for( const LocalSlotObject& slotObject : localSlot->slotObjects )
    {
        slotObject.functionPointer( serializedParameters, packet );

        // Not threadsafe
        if( interruptSignal == true )
            break;

        serializedParameters->ResetReadPointer();
    }
}

void RPC4::InterruptSignal( void )
{
    interruptSignal = true;
}

void RPC4::OnAttach( void )
{
    // A registration refused because its name is already registered, by an earlier attach or by
    // the application, leaves the function already registered under it in place.
    for( const GlobalRegistration& registration : GlobalRegistrations() )
    {
        const char* functionName = registration.functionName.c_str();
        if( registration.registerFunctionPointer )
        {
            if( registration.callPriority == (int)0xFFFFFFFF )
                std::ignore = RegisterFunction( functionName, registration.registerFunctionPointer );
            else
                RegisterSlot( functionName, registration.registerFunctionPointer, registration.callPriority );
        }
        else if( registration.registerBlockingFunctionPointer )
            std::ignore = RegisterBlockingFunction( functionName, registration.registerBlockingFunctionPointer );
        else
            RegisterLocalCallback( functionName, registration.messageId );
    }
}

PluginReceiveResult RPC4::OnReceive( Packet* packet )
{
    if( packet->data[0] == ID_RPC_PLUGIN )
    {
        BitStream bsIn( packet->data, packet->length, false );
        bsIn.IgnoreBytes( 2 );

        if( packet->data[1] == ID_RPC4_CALL )
        {
            std::string functionName;
            bool isBlocking = false;
            if( !bsIn.ReadCompressed( functionName ) || !bsIn.Read( isBlocking ) )
                return RR_STOP_PROCESSING_AND_DEALLOCATE;
            if( isBlocking == false )
            {
                auto it = registeredNonblockingFunctions.find( functionName );
                if( it == registeredNonblockingFunctions.end() )
                {
                    BitStream bsOut;
                    bsOut.Write( (unsigned char)ID_RPC_REMOTE_ERROR );
                    bsOut.Write( (unsigned char)RPC_ERROR_FUNCTION_NOT_REGISTERED );
                    bsOut.Write( functionName );
                    SendUnified( &bsOut, HIGH_PRIORITY, RELIABLE_ORDERED, 0, packet->systemAddress, false );
                    return RR_STOP_PROCESSING_AND_DEALLOCATE;
                }

                void ( *fp )( BitStream*, Packet* ) = it->second;
                bsIn.AlignReadToByteBoundary();
                fp( &bsIn, packet );
            }
            else
            {
                auto it = registeredBlockingFunctions.find( functionName );
                if( it == registeredBlockingFunctions.end() )
                {
                    BitStream bsOut;
                    bsOut.Write( (unsigned char)ID_RPC_REMOTE_ERROR );
                    bsOut.Write( (unsigned char)RPC_ERROR_FUNCTION_NOT_REGISTERED );
                    bsOut.Write( functionName );
                    SendUnified( &bsOut, HIGH_PRIORITY, RELIABLE_ORDERED, 0, packet->systemAddress, false );
                    return RR_STOP_PROCESSING_AND_DEALLOCATE;
                }

                void ( *fp )( BitStream*, BitStream*, Packet* ) = it->second;
                BitStream returnData;
                bsIn.AlignReadToByteBoundary();
                fp( &bsIn, &returnData, packet );

                BitStream out;
                out.Write( (MessageID)ID_RPC_PLUGIN );
                out.Write( (MessageID)ID_RPC4_RETURN );
                returnData.ResetReadPointer();
                out.AlignWriteToByteBoundary();
                out.Write( returnData );
                SendUnified( &out, IMMEDIATE_PRIORITY, RELIABLE_ORDERED, 0, packet->systemAddress, false );
            }
        }
        else if( packet->data[1] == ID_RPC4_SIGNAL )
        {
            std::string sharedIdentifier;
            if( !bsIn.ReadCompressed( sharedIdentifier ) )
                return RR_STOP_PROCESSING_AND_DEALLOCATE;
            BitStream serializedParameters;
            bsIn.AlignReadToByteBoundary();
            // Copies whatever is left, so it cannot fail.
            std::ignore = bsIn.Read( &serializedParameters );
            if( auto it = localSlots.find( sharedIdentifier ); it != localSlots.end() )
            {
                InvokeSignal( it->second.get(), &serializedParameters, packet );
            }
        }
        else
        {
            RakAssert( packet->data[1] == ID_RPC4_RETURN );
            blockingReturnValue.Reset();
            blockingReturnValue.Write( bsIn );
            gotBlockingReturnValue = true;
        }

        return RR_STOP_PROCESSING_AND_DEALLOCATE;
    }

    if( const auto callbackIt = localCallbacks.find( packet->data[0] ); callbackIt != localCallbacks.end() )
    {
        // Copied, because a function may unregister a local callback while the loop runs.
        const std::set<std::string> functions = callbackIt->second->functions;
        for( const std::string& function : functions )
        {
            BitStream bsIn( packet->data, packet->length, false );

            if( auto it = registeredNonblockingFunctions.find( function ); it != registeredNonblockingFunctions.end() )
            {
                void ( *fp )( BitStream*, Packet* ) = it->second;
                bsIn.AlignReadToByteBoundary();
                fp( &bsIn, packet );
            }
        }
    }

    return RR_CONTINUE_PROCESSING;
}

} // namespace RakNet

#endif // _RAKNET_SUPPORT_*
