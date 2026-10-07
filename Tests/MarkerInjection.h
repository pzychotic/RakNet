#pragma once

#include "MessageIdentifiers.h"
#include "RakNetTime.h"
#include "RakNetTypes.h"

#include <algorithm>
#include <vector>

namespace RakNet {
class BitStream;
class RakPeerInterface;
} // namespace RakNet

/*
 *  The suite's check of which crafted Messages a plugin lets through: send them, then a
 *  marker, and read what the target's Receive hands out before the marker.
 *
 *  Everything goes RELIABLE_ORDERED on channel 0, so the marker comes out of Receive after
 *  every Message sent ahead of it, and Receive hands a Message out only after every attached
 *  plugin has seen it. Once the marker is out, the plugin has handled each crafted Message,
 *  and the ones it did not consume are in the returned list.
 *
 *  The same holds for a reply the target's plugin sends back: a marker sent afterwards in the
 *  other direction, with no Messages ahead of it, comes out after the reply.
 */
namespace MarkerInjection {

constexpr RakNet::MessageID kMarker = RakNet::ID_USER_PACKET_ENUM;

// Hang guard for the marker. On loopback it arrives a few update cycles after the send, tens
// of milliseconds.
constexpr RakNet::TimeMS kMarkerBudget = 5000;

// Sends each of messages from sender to target in order, then the marker, and receives on
// target until the marker comes out. Returns the first byte of every Message target's Receive
// handed out before it; Messages after it stay queued. FAILs if the marker does not come out
// within kMarkerBudget.
//
// Receives only on target; sender's queue is left alone, so a reply to sender can still be
// waited for afterwards.
std::vector<RakNet::MessageID> Inject( RakNet::RakPeerInterface* sender, RakNet::RakPeerInterface* target, const std::vector<RakNet::BitStream*>& messages );

std::vector<RakNet::MessageID> Inject( RakNet::RakPeerInterface* sender, RakNet::RakPeerInterface* target, RakNet::BitStream& message );

inline bool Contains( const std::vector<RakNet::MessageID>& ids, RakNet::MessageID id )
{
    return std::find( ids.begin(), ids.end(), id ) != ids.end();
}

} // namespace MarkerInjection
