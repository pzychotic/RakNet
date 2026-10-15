#include "Plugins/RPC4Plugin.h"

#include "BitStream.h"
#include "MessageIdentifiers.h"
#include "RakNetTypes.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

/*
RPC4GlobalRegistration queues a registration that every RPC4 replays in OnAttach.

The registrations below are namespace-scope statics, so they run during static
initialisation in no particular order relative to RPC4Plugin.cpp. The queue lives
for the whole test binary and every RPC4 attached anywhere in it sees these entries,
so every name carries this file's prefix.

AttachPlugin calls OnAttach without Startup, so an unstarted peer is the whole
fixture. Each case checks that the matching Unregister call finds the name.
*/

using namespace RakNet;

namespace {

void NonBlocking( BitStream*, Packet* ) {}
void Blocking( BitStream*, BitStream*, Packet* ) {}

const MessageID kLocalCallbackId = ID_USER_PACKET_ENUM + 1;
const int kManyCount = 64;

std::string ManyName( int index )
{
    return "RPC4GlobalRegistrationTest.Many." + std::to_string( index );
}

std::string LongName()
{
    return "RPC4GlobalRegistrationTest.Long." + std::string( 200, 'x' );
}

const RPC4GlobalRegistration functionRegistration( "RPC4GlobalRegistrationTest.Function", NonBlocking );
const RPC4GlobalRegistration slotRegistration( "RPC4GlobalRegistrationTest.Slot", NonBlocking, 5 );
const RPC4GlobalRegistration blockingRegistration( "RPC4GlobalRegistrationTest.Blocking", Blocking );
const RPC4GlobalRegistration localCallbackRegistration( "RPC4GlobalRegistrationTest.LocalCallback", kLocalCallbackId );
const RPC4GlobalRegistration longNameRegistration( LongName().c_str(), NonBlocking );

// Each constructor copies its name into the queue, so a registration need not
// outlive its constructor.
struct ManyRegistrations
{
    ManyRegistrations()
    {
        for( int i = 0; i < kManyCount; i++ )
            const RPC4GlobalRegistration registration( ManyName( i ).c_str(), NonBlocking );
    }
};
const ManyRegistrations manyRegistrations;

// An RPC4 attached to a peer that was never started.
struct AttachedRPC4
{
    AttachedRPC4()
    : peer( RakPeerInterface::GetInstance() )
    {
        peer->AttachPlugin( &rpc4 );
    }
    ~AttachedRPC4()
    {
        peer->DetachPlugin( &rpc4 );
        RakPeerInterface::DestroyInstance( peer );
    }
    AttachedRPC4( const AttachedRPC4& ) = delete;
    AttachedRPC4& operator=( const AttachedRPC4& ) = delete;

    RPC4 rpc4;
    RakPeerInterface* peer;
};

} // namespace

TEST_CASE( "RPC4GlobalRegistration registers a function on attach", "[rpc4]" )
{
    AttachedRPC4 attached;
    CHECK( attached.rpc4.UnregisterFunction( "RPC4GlobalRegistrationTest.Function" ) );
}

TEST_CASE( "RPC4GlobalRegistration registers a slot on attach", "[rpc4]" )
{
    AttachedRPC4 attached;
    CHECK( attached.rpc4.UnregisterSlot( "RPC4GlobalRegistrationTest.Slot" ) );
    CHECK_FALSE( attached.rpc4.UnregisterFunction( "RPC4GlobalRegistrationTest.Slot" ) );
}

TEST_CASE( "RPC4GlobalRegistration registers a blocking function on attach", "[rpc4]" )
{
    AttachedRPC4 attached;
    CHECK( attached.rpc4.UnregisterBlockingFunction( "RPC4GlobalRegistrationTest.Blocking" ) );
}

TEST_CASE( "RPC4GlobalRegistration registers a local callback on attach", "[rpc4]" )
{
    AttachedRPC4 attached;
    CHECK( attached.rpc4.UnregisterLocalCallback( "RPC4GlobalRegistrationTest.LocalCallback", kLocalCallbackId ) );
}

TEST_CASE( "RPC4GlobalRegistration keeps a 200-character name whole", "[rpc4]" )
{
    AttachedRPC4 attached;
    CHECK( attached.rpc4.UnregisterFunction( LongName().c_str() ) );
}

TEST_CASE( "RPC4GlobalRegistration takes 64 registrations", "[rpc4]" )
{
    AttachedRPC4 attached;
    for( int i = 0; i < kManyCount; i++ )
    {
        INFO( ManyName( i ) );
        CHECK( attached.rpc4.UnregisterFunction( ManyName( i ).c_str() ) );
    }
}
