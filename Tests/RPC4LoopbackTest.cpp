#include "Plugins/RPC4Plugin.h"

#include "BitStream.h"
#include "MessageIdentifiers.h"
#include "PeerScope.h"
#include "RakNetTypes.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

/*
CallLoopback's error path, driven locally.

Calling an unregistered function through CallLoopback does not send anything: RPC4
builds an ID_RPC_REMOTE_ERROR packet itself and pushes it onto the peer's own
receive queue. So a started peer with nothing connected to it is the whole fixture
- no second peer, no socket traffic, no waiting - which is why this file is tagged
[rpc4] and not [network]. The peer still has to be started, because
RakPeer::Receive returns nothing while inactive.

The regression: that packet used to be built with strcpy, a raw C string with a
terminator and no length prefix, while both the network writer
(RPC4::OnReceive) and the only reader (RPC4::CallBlocking's drain loop) use
BitStream::Write/Read on a std::string - a uint16 length followed by the bytes.
The reader consumed the first two characters of the function name as that length
and then ran off the end of the packet. Reading the packet back the way
CallBlocking does is the test.
*/

using namespace RakNet;

TEST_CASE( "RPC4 CallLoopback error packet is readable by the RPC4 reader", "[rpc4]" )
{
    // Before the PeerScope, deliberately, so it is destroyed after it: nothing
    // detaches a plugin on destruction, and DestroyInstance runs Shutdown, which
    // calls OnRakPeerShutdown on everything still in the peer's plugin list. A
    // failing REQUIRE below throws straight past the DetachPlugin at the bottom,
    // and this order is what keeps that path from calling into a dead plugin.
    RPC4 rpc4;

    PeerScope peers;
    RakPeerInterface* peer = peers.Client();
    peer->AttachPlugin( &rpc4 );

    const std::string uniqueID = "NoSuchFunction";
    rpc4.CallLoopback( uniqueID.c_str(), nullptr );

    Packet* packet = peer->Receive();
    REQUIRE( packet != nullptr );

    // Fields the drain loop in CallBlocking dispatches on before it parses.
    REQUIRE( packet->length >= 2 );
    CHECK( packet->data[0] == ID_RPC_REMOTE_ERROR );
    CHECK( packet->data[1] == RPC_ERROR_FUNCTION_NOT_REGISTERED );

    // Verbatim from CallBlocking's ID_RPC_REMOTE_ERROR branch.
    std::string functionName;
    BitStream bsIn( packet->data, packet->length, false );
    bsIn.IgnoreBytes( 2 );
    bsIn.Read( functionName );

    CHECK( functionName == uniqueID );

    peer->DeallocatePacket( packet );
    peer->DetachPlugin( &rpc4 );
}
