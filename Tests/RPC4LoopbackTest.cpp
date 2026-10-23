#include "Plugins/RPC4Plugin.h"

#include "BitStream.h"
#include "MessageIdentifiers.h"
#include "PeerScope.h"
#include "RakNetTypes.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

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

    // The reads of CallBlocking's ID_RPC_REMOTE_ERROR branch.
    std::string functionName;
    BitStream bsIn( packet->data, packet->length, false );
    bsIn.IgnoreBytes( 2 );
    REQUIRE( bsIn.Read( functionName ) );

    CHECK( functionName == uniqueID );

    peer->DeallocatePacket( packet );
    peer->DetachPlugin( &rpc4 );
}

namespace {

std::vector<char> g_slotCalls;

void SlotA( BitStream*, Packet* ) { g_slotCalls.push_back( 'A' ); }
void SlotB( BitStream*, Packet* ) { g_slotCalls.push_back( 'B' ); }
void SlotC( BitStream*, Packet* ) { g_slotCalls.push_back( 'C' ); }

} // namespace

TEST_CASE( "RPC4 calls a signal's slots by priority, highest first, then in registration order", "[rpc4]" )
{
    RPC4 rpc4;

    PeerScope peers;
    RakPeerInterface* peer = peers.Client();
    peer->AttachPlugin( &rpc4 );

    rpc4.RegisterSlot( "signal", SlotA, 0 );
    rpc4.RegisterSlot( "signal", SlotB, 5 );
    rpc4.RegisterSlot( "signal", SlotC, 0 );

    g_slotCalls.clear();
    rpc4.Signal( "signal", nullptr, HIGH_PRIORITY, RELIABLE_ORDERED, 0, UNASSIGNED_SYSTEM_ADDRESS, true, true );

    CHECK( g_slotCalls == std::vector<char>{ 'B', 'A', 'C' } );

    peer->DetachPlugin( &rpc4 );
}

namespace {

std::vector<char> g_functionCalls;

void FunctionA( BitStream*, Packet* ) { g_functionCalls.push_back( 'A' ); }
void FunctionB( BitStream*, Packet* ) { g_functionCalls.push_back( 'B' ); }

void BlockingFunctionA( BitStream*, BitStream*, Packet* ) {}
void BlockingFunctionB( BitStream*, BitStream*, Packet* ) {}

} // namespace

TEST_CASE( "RPC4 refuses a function name already registered and keeps the first function", "[rpc4]" )
{
    RPC4 rpc4;

    PeerScope peers;
    RakPeerInterface* peer = peers.Client();
    peer->AttachPlugin( &rpc4 );

    CHECK( rpc4.RegisterFunction( "function", FunctionA ) );
    CHECK_FALSE( rpc4.RegisterFunction( "function", FunctionB ) );

    g_functionCalls.clear();
    rpc4.CallLoopback( "function", nullptr );
    // The plugin runs the function as Receive passes the loopback packet to it.
    Packet* packet = peer->Receive();
    if( packet != nullptr )
        peer->DeallocatePacket( packet );

    CHECK( g_functionCalls == std::vector<char>{ 'A' } );

    peer->DetachPlugin( &rpc4 );
}

TEST_CASE( "RPC4 refuses a blocking function name already registered until it is unregistered", "[rpc4]" )
{
    RPC4 rpc4;

    CHECK( rpc4.RegisterBlockingFunction( "function", BlockingFunctionA ) );
    CHECK_FALSE( rpc4.RegisterBlockingFunction( "function", BlockingFunctionB ) );

    CHECK( rpc4.UnregisterBlockingFunction( "function" ) );
    CHECK( rpc4.RegisterBlockingFunction( "function", BlockingFunctionB ) );
}
