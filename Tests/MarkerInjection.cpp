#include "MarkerInjection.h"

#include "ConnectionWaits.h"

#include "BitStream.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

using namespace RakNet;

std::vector<MessageID> MarkerInjection::Inject( RakPeerInterface* sender, RakPeerInterface* target, const std::vector<BitStream*>& messages )
{
    const RakNetGUID targetGuid = target->GetMyGUID();
    for( BitStream* message : messages )
    {
        sender->Send( message, HIGH_PRIORITY, RELIABLE_ORDERED, 0, targetGuid, false );
    }

    BitStream marker;
    marker.Write( kMarker );
    sender->Send( &marker, HIGH_PRIORITY, RELIABLE_ORDERED, 0, targetGuid, false );

    std::vector<MessageID> received;
    const bool markerCame = ConnectionWaits::WaitUntil(
        [&] {
            for( Packet* packet = target->Receive(); packet != nullptr; packet = target->Receive() )
            {
                const bool isMarker = packet->length > 0 && packet->data[0] == kMarker;
                if( !isMarker && packet->length > 0 )
                {
                    received.push_back( packet->data[0] );
                }
                target->DeallocatePacket( packet );
                if( isMarker )
                {
                    return true;
                }
            }
            return false;
        },
        kMarkerBudget );

    if( !markerCame )
    {
        FAIL( "the marker did not come out of the target's Receive within " << kMarkerBudget << " ms" );
    }
    return received;
}

std::vector<MessageID> MarkerInjection::Inject( RakPeerInterface* sender, RakPeerInterface* target, BitStream& message )
{
    return Inject( sender, target, std::vector<BitStream*>{ &message } );
}
