#include "MTUSize.h"
#include "PluginInterface2.h"

#include <catch2/catch_test_macros.hpp>

#include <climits>
#include <string>
#include <vector>

/*
Pins SendListUnified's loopback branch: a plugin attached to neither a RakPeerInterface nor
a TCPInterface delivers a list sent to its own GUID straight to its OnReceive. With nothing
attached that GUID is UNASSIGNED_RAKNET_GUID, so a default AddressOrGUID matches.

The concatenation obeys the same limit as RakPeer::SendList: the lengths are summed without
wrapping, and a total above MAXIMUM_MESSAGE_SIZE is rejected by return value before
anything is allocated or delivered.

No peer is started and no port is bound.
*/

using namespace RakNet;

namespace {

// Attached to nothing, so SendListUnified takes the loopback branch.
class LoopbackPlugin : public PluginInterface2
{
public:
    bool SendList( const char** data, const int* lengths, int numParameters )
    {
        return SendListUnified( data, lengths, numParameters, HIGH_PRIORITY, RELIABLE_ORDERED, 0, AddressOrGUID(), false );
    }

    PluginReceiveResult OnReceive( Packet* packet ) override
    {
        received.emplace_back( (const char*)packet->data, packet->length );
        return RR_CONTINUE_PROCESSING;
    }

    std::vector<std::string> received;
};

} // namespace

TEST_CASE( "SendListUnified rejects int lengths that sum past UINT32_MAX", "[plugininterface2]" )
{
    const char blocks[3] = { 'a', 'b', 'c' };
    const char* data[] = { &blocks[0], &blocks[1], &blocks[2] };
    const int lengths[] = { INT32_MAX, INT32_MAX, 3 };

    LoopbackPlugin plugin;
    CHECK_FALSE( plugin.SendList( data, lengths, 3 ) );
    CHECK( plugin.received.empty() );
}

TEST_CASE( "SendListUnified rejects a total one byte over MAXIMUM_MESSAGE_SIZE", "[plugininterface2]" )
{
    // Real buffers of the claimed sizes, so a missing check delivers instead of overreading.
    const std::vector<char> block( MAXIMUM_MESSAGE_SIZE / 2 + 1, 'x' );
    const char* data[] = { block.data(), block.data() };
    const int lengths[] = { (int)( MAXIMUM_MESSAGE_SIZE / 2 ), (int)( MAXIMUM_MESSAGE_SIZE / 2 + 1 ) };

    LoopbackPlugin plugin;
    CHECK_FALSE( plugin.SendList( data, lengths, 2 ) );
    CHECK( plugin.received.empty() );
}

TEST_CASE( "SendListUnified delivers the concatenation of the positive-length blocks in order", "[plugininterface2]" )
{
    const char* data[] = { "ab", "skipped", "cde", "also skipped" };
    const int lengths[] = { 2, 0, 3, -1 };

    LoopbackPlugin plugin;
    CHECK( plugin.SendList( data, lengths, 4 ) );
    REQUIRE( plugin.received.size() == 1 );
    CHECK( plugin.received[0] == "abcde" );
}

TEST_CASE( "SendListUnified sends nothing when every block is empty", "[plugininterface2]" )
{
    const char* data[] = { "a", "b" };
    const int lengths[] = { 0, -5 };

    LoopbackPlugin plugin;
    CHECK_FALSE( plugin.SendList( data, lengths, 2 ) );
    CHECK( plugin.received.empty() );
}
