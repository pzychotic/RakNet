#include "Plugins/MessageFilter.h"

#include "BitStream.h"
#include "MessageIdentifiers.h"
#include "RakNetTypes.h"
#include "StringCompressorScope.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

/*
MessageFilter's RPC4 allow-list, driven directly rather than over a socket.

OnReceive is public and virtual, and nothing on the path taken here touches
rakPeerInterface: a default FilterSet neither kicks nor bans, and both of those
branches are null guarded anyway. So the whole filter decision can be exercised by
handing OnReceive a Packet built by hand, with no peer, no socket and no waiting -
which is why this file is tagged [messagefilter] and not [network].

The packet layout is RPC4's, from RPC4::Call: ID_RPC_PLUGIN, then the RPC4
sub-identifier, the function name via WriteCompressed, then the nonblocking flag.
MessageFilter skips the two identifier bytes and reads the name back.
*/

using namespace RakNet;

namespace {

// Private to RPC4Plugin.cpp, so it cannot be included; RPC4::Call writes this
// value as the second byte of every call it sends.
constexpr unsigned char kRPC4Call = 0;

constexpr int kFilterSetID = 0;

// A Packet whose data it owns, so the bytes outlive the BitStream that built them.
class RPC4CallPacket
{
public:
    explicit RPC4CallPacket( const std::string& functionName )
    {
        BitStream out;
        out.Write( static_cast<MessageID>( ID_RPC_PLUGIN ) );
        out.Write( static_cast<MessageID>( kRPC4Call ) );
        out.WriteCompressed( functionName );

        // RPC4::Call writes the nonblocking flag straight after the name, so the
        // name is not the last field on the real wire. It is here so a decoder that
        // stops on the wrong bit runs into a following field rather than the end of
        // the packet - the same reason BitStreamRoundTripTest writes a sentinel.
        out.Write( false );

        bytes.assign( out.GetData(), out.GetData() + out.GetNumberOfBytesUsed() );

        packet.systemAddress = SystemAddress( "1.2.3.4", 60000 );
        packet.guid = UNASSIGNED_RAKNET_GUID;
        packet.length = static_cast<unsigned int>( bytes.size() );
        packet.bitSize = out.GetNumberOfBitsUsed();
        packet.data = bytes.data();
        packet.deleteData = false;
        packet.wasGeneratedLocally = false;
    }

    Packet* Get( void ) { return &packet; }

private:
    std::vector<unsigned char> bytes;
    Packet packet{};
};

} // namespace

TEST_CASE( "MessageFilter lets through an RPC4 call that is on the allow list", "[messagefilter]" )
{
    // The regression. The handler used to read a single run-length-compressed byte
    // into a default-constructed std::string instead of decoding the name, so the
    // lookup was always for "" and every RPC4 call - allowed or not - was dropped.
    StringCompressorScope compressor;

    RPC4CallPacket call( "AllowedFunction" );

    MessageFilter filter;
    filter.SetAllowRPC4( true, "AllowedFunction", kFilterSetID );
    filter.SetSystemFilterSet( call.Get(), kFilterSetID );

    CHECK( filter.OnReceive( call.Get() ) == RR_CONTINUE_PROCESSING );
}

TEST_CASE( "MessageFilter drops an RPC4 call that is not on the allow list", "[messagefilter]" )
{
    // The other half: reading the name correctly must not turn the allow list into
    // a pass-through. Same setup, a name that was never allowed.
    StringCompressorScope compressor;

    RPC4CallPacket call( "ForbiddenFunction" );

    MessageFilter filter;
    filter.SetAllowRPC4( true, "AllowedFunction", kFilterSetID );
    filter.SetSystemFilterSet( call.Get(), kFilterSetID );

    CHECK( filter.OnReceive( call.Get() ) == RR_STOP_PROCESSING_AND_DEALLOCATE );
}
