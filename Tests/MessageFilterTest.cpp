#include "Plugins/MessageFilter.h"

#include "BitStream.h"
#include "GetTime.h"
#include "MessageIdentifiers.h"
#include "RakNetTypes.h"
#include "StringCompressorScope.h"
#include "TCPInterface.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

/*
MessageFilter's allow-lists, driven directly rather than over a socket.

OnReceive is public and virtual, and nothing on the path taken here touches
rakPeerInterface: a default FilterSet neither kicks nor bans, and both of those
branches are null guarded anyway. So the whole filter decision can be exercised by
handing OnReceive a Packet built by hand, with no peer, no socket and no waiting -
which is why this file is tagged [messagefilter] and not [network].

The RPC4 packet layout is RPC4::Call's: ID_RPC_PLUGIN, then the RPC4
sub-identifier, the function name via WriteCompressed, then the nonblocking flag.
MessageFilter skips the two identifier bytes and reads the name back.

A Timestamped Message is ID_TIMESTAMP, a RakNet::Time, then its Message ID. RakPeer
has already shifted the time by the time any plugin sees it, so the bytes built here
are what OnReceive gets on the real path.
*/

using namespace RakNet;

namespace {

// Private to RPC4Plugin.cpp, so it cannot be included; RPC4::Call writes this
// value as the second byte of every call it sends.
constexpr unsigned char kRPC4Call = 0;

constexpr int kFilterSetID = 0;

// A Packet whose data it owns, so the bytes outlive the BitStream that built them,
// from a System the tests can filter or leave alone.
class RawPacket
{
public:
    explicit RawPacket( std::vector<unsigned char> data, const char* address = "1.2.3.4" )
    : bytes( std::move( data ) )
    {
        packet.systemAddress = SystemAddress( address, 30000 );
        packet.guid = UNASSIGNED_RAKNET_GUID;
        packet.length = static_cast<unsigned int>( bytes.size() );
        packet.bitSize = BytesToBits( packet.length );
        packet.data = bytes.data();
        packet.deleteData = false;
        packet.wasGeneratedLocally = false;
    }

    Packet* Get( void ) { return &packet; }

private:
    std::vector<unsigned char> bytes;
    Packet packet{};
};

std::vector<unsigned char> RPC4CallBytes( const std::string& functionName )
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

    return std::vector<unsigned char>( out.GetData(), out.GetData() + out.GetNumberOfBytesUsed() );
}

// The byte at offset 5 of a Timestamped Message, the fifth byte of its big-endian time.
constexpr size_t kTimeByteOffset = sizeof( MessageID ) + sizeof( RakNet::TimeMS );

// ID_TIMESTAMP, then a time whose fifth byte is timeByte, then messageId if given.
std::vector<unsigned char> TimestampedBytes( unsigned char timeByte, std::optional<MessageID> messageId )
{
    const RakNet::Time time = static_cast<RakNet::Time>( timeByte ) << 24 | 0x0000000700020304ull;

    BitStream out;
    out.Write( static_cast<MessageID>( ID_TIMESTAMP ) );
    out.Write( time );
    if( messageId )
        out.Write( *messageId );

    std::vector<unsigned char> bytes( out.GetData(), out.GetData() + out.GetNumberOfBytesUsed() );
    REQUIRE( bytes.size() > kTimeByteOffset );
    REQUIRE( bytes[kTimeByteOffset] == timeByte );
    return bytes;
}

struct DisallowedCalls
{
    int count = 0;
    unsigned char lastID = 0;
};

void RecordDisallowed( RakPeerInterface*, AddressOrGUID, int, void* userData, unsigned char messageID )
{
    auto* calls = static_cast<DisallowedCalls*>( userData );
    ++calls->count;
    calls->lastID = messageID;
}

constexpr MessageID kBlockedID = ID_USER_PACKET_ENUM;
constexpr MessageID kAllowedID = ID_USER_PACKET_ENUM + 1;
// A third ID the tests put in the time, so reading the wrong byte flips each answer.
constexpr MessageID kDecoyID = ID_USER_PACKET_ENUM + 2;

} // namespace

TEST_CASE( "MessageFilter lets through an RPC4 call that is on the allow list", "[messagefilter]" )
{
    // The regression. The handler used to read a single run-length-compressed byte
    // into a default-constructed std::string instead of decoding the name, so the
    // lookup was always for "" and every RPC4 call - allowed or not - was dropped.
    StringCompressorScope compressor;

    RawPacket call( RPC4CallBytes( "AllowedFunction" ) );

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

    RawPacket call( RPC4CallBytes( "ForbiddenFunction" ) );

    MessageFilter filter;
    filter.SetAllowRPC4( true, "AllowedFunction", kFilterSetID );
    filter.SetSystemFilterSet( call.Get(), kFilterSetID );

    CHECK( filter.OnReceive( call.Get() ) == RR_STOP_PROCESSING_AND_DEALLOCATE );
}

TEST_CASE( "MessageFilter judges a Timestamped Message by the Message ID after its time", "[messagefilter]" )
{
    RawPacket message( TimestampedBytes( kDecoyID, kBlockedID ) );

    MessageFilter filter;
    DisallowedCalls calls;
    filter.SetAllowMessageID( true, kDecoyID, kDecoyID, kFilterSetID );
    filter.SetDisallowedMessageCallback( kFilterSetID, &calls, RecordDisallowed );
    filter.SetSystemFilterSet( message.Get(), kFilterSetID );

    CHECK( filter.OnReceive( message.Get() ) == RR_STOP_PROCESSING_AND_DEALLOCATE );
    CHECK( calls.count == 1 );
    CHECK( calls.lastID == kBlockedID );
}

TEST_CASE( "MessageFilter lets through a Timestamped Message whose Message ID is allowed", "[messagefilter]" )
{
    // The decoy in the time is disallowed here, so a filter reading it would drop this.
    RawPacket message( TimestampedBytes( kDecoyID, kAllowedID ) );

    MessageFilter filter;
    DisallowedCalls calls;
    filter.SetAllowMessageID( true, kAllowedID, kAllowedID, kFilterSetID );
    filter.SetDisallowedMessageCallback( kFilterSetID, &calls, RecordDisallowed );
    filter.SetSystemFilterSet( message.Get(), kFilterSetID );

    CHECK( filter.OnReceive( message.Get() ) == RR_CONTINUE_PROCESSING );
    CHECK( calls.count == 0 );
}

TEST_CASE( "MessageFilter disallows a filtered System's Timestamped Message too short for a Message ID", "[messagefilter]" )
{
    // A full time with no Message ID after it, and one cut off inside the time.
    std::vector<unsigned char> full = TimestampedBytes( kDecoyID, std::nullopt );
    std::vector<unsigned char> cut( full.begin(), full.begin() + 3 );
    auto bytes = GENERATE_COPY( values( { full, cut } ) );
    RawPacket message( bytes );

    MessageFilter filter;
    DisallowedCalls calls;
    filter.SetAllowMessageID( true, kDecoyID, kDecoyID, kFilterSetID );
    // Allowing ID_TIMESTAMP itself does not let through a Message with no ID to judge.
    filter.SetAllowMessageID( true, ID_TIMESTAMP, ID_TIMESTAMP, kFilterSetID );
    filter.SetDisallowedMessageCallback( kFilterSetID, &calls, RecordDisallowed );
    filter.SetSystemFilterSet( message.Get(), kFilterSetID );

    CHECK( filter.OnReceive( message.Get() ) == RR_STOP_PROCESSING_AND_DEALLOCATE );
    CHECK( calls.count == 1 );
    CHECK( calls.lastID == ID_TIMESTAMP );
}

TEST_CASE( "MessageFilter lets through an unfiltered System's short Timestamped Message", "[messagefilter]" )
{
    std::vector<unsigned char> full = TimestampedBytes( kDecoyID, std::nullopt );
    std::vector<unsigned char> cut( full.begin(), full.begin() + 3 );
    auto bytes = GENERATE_COPY( values( { full, cut } ) );
    RawPacket message( bytes, "1.2.3.4" );
    RawPacket filteredSystem( { ID_USER_PACKET_ENUM }, "5.6.7.8" );

    // A filter on another System, so the filter is not simply empty.
    MessageFilter filter;
    filter.SetSystemFilterSet( filteredSystem.Get(), kFilterSetID );

    CHECK( filter.OnReceive( message.Get() ) == RR_CONTINUE_PROCESSING );
}

TEST_CASE( "MessageFilter lists its filter sets in ID order and keeps the others when one is deleted", "[messagefilter]" )
{
    MessageFilter filter;
    filter.SetAllowMessageID( true, ID_USER_PACKET_ENUM, ID_USER_PACKET_ENUM, 7 );
    filter.SetAllowMessageID( true, ID_USER_PACKET_ENUM, ID_USER_PACKET_ENUM, 3 );
    filter.SetAllowMessageID( true, ID_USER_PACKET_ENUM, ID_USER_PACKET_ENUM, 5 );

    const auto filterSetIDs = [&filter] {
        std::vector<int> ids;
        for( unsigned index = 0; index < filter.GetFilterSetCount(); index++ )
            ids.push_back( filter.GetFilterSetIDByIndex( index ) );
        return ids;
    };

    CHECK( filterSetIDs() == std::vector<int>{ 3, 5, 7 } );

    filter.DeleteFilterSet( 5 );

    CHECK( filterSetIDs() == std::vector<int>{ 3, 7 } );
}

namespace {

// Update checks timeouts at most once a second, counted from construction. Rewinding
// the last check lets a test run one straight away.
class CheckableMessageFilter : public MessageFilter
{
public:
    void MakeTimeoutCheckDue( void ) { whenLastTimeoutCheck = RakNet::GetTime() - 2000; }
};

struct TimeoutCalls
{
    std::vector<SystemAddress> systems;
    MessageFilter* deleteSetFrom = nullptr;
};

void RecordTimeout( RakPeerInterface*, AddressOrGUID system, int filterSetID, void* userData )
{
    auto* calls = static_cast<TimeoutCalls*>( userData );
    calls->systems.push_back( system.systemAddress );
    if( calls->deleteSetFrom )
        calls->deleteSetFrom->DeleteFilterSet( filterSetID );
}

// An expired System is kicked through whichever interface the plugin is attached to,
// and a bare plugin has neither. A TCPInterface that was never started takes the kick
// and does nothing, and leaves rakPeerInterface null. The timeout callbacks record into
// calls, which lives as long as the filter that calls them.
struct UpdateRig
{
    UpdateRig() { tcp.AttachPlugin( &filter ); }
    ~UpdateRig() { tcp.DetachPlugin( &filter ); }

    CheckableMessageFilter filter;
    TCPInterface tcp;
    TimeoutCalls calls;
};

// A timeout check over a few Systems returns in well under a millisecond, so a check
// still running after this long never returns.
constexpr std::chrono::seconds kUpdateReturnBudget( 5 );

// Runs one timeout check on another thread and waits for it. A check that never returns
// can't be joined or stopped, so the thread is detached and the rig it is still using is
// leaked to it. The test fails and the rest of the run goes on.
bool UpdateReturns( std::unique_ptr<UpdateRig>& rig )
{
    rig->filter.MakeTimeoutCheckDue();

    auto returned = std::make_shared<std::promise<void>>();
    std::future<void> future = returned->get_future();
    UpdateRig* raw = rig.get();
    std::thread( [raw, returned] {
        raw->filter.Update();
        returned->set_value();
    } ).detach();

    if( future.wait_for( kUpdateReturnBudget ) == std::future_status::ready )
        return true;

    (void)rig.release();
    return false;
}

constexpr int kLiveSetID = 1;
constexpr int kExpiringSetID = 2;

const SystemAddress kLiveSystem( "1.2.3.4", 30000 );
const SystemAddress kExpiredSystem( "5.6.7.8", 30000 );
const SystemAddress kOtherExpiredSystem( "9.10.11.12", 30000 );

// Gives kExpiringSetID a 1 ms limit and a timeout callback that records into rig.calls,
// puts kLiveSystem in a set with no limit and expiringSystems in kExpiringSetID, and
// waits out the limit.
void ExpireSystems( UpdateRig& rig, const std::vector<SystemAddress>& expiringSystems )
{
    rig.filter.SetFilterMaxTime( 1, false, 0, kExpiringSetID );
    rig.filter.SetTimeoutCallback( kExpiringSetID, &rig.calls, RecordTimeout );
    rig.filter.SetSystemFilterSet( kLiveSystem, kLiveSetID );
    for( const SystemAddress& system : expiringSystems )
        rig.filter.SetSystemFilterSet( system, kExpiringSetID );

    const RakNet::TimeMS start = RakNet::GetTimeMS();
    while( RakNet::GetTimeMS() - start < 2 )
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
}

} // namespace

TEST_CASE( "MessageFilter's timeout check returns with a System in a set that has no time limit", "[messagefilter]" )
{
    auto rig = std::make_unique<UpdateRig>();
    rig->filter.SetSystemFilterSet( kLiveSystem, kLiveSetID );

    REQUIRE( UpdateReturns( rig ) );

    CHECK( rig->filter.GetSystemFilterSet( kLiveSystem ) == kLiveSetID );
}

TEST_CASE( "MessageFilter's timeout check removes the expired System and keeps the live one", "[messagefilter]" )
{
    auto rig = std::make_unique<UpdateRig>();
    ExpireSystems( *rig, { kExpiredSystem } );

    REQUIRE( UpdateReturns( rig ) );

    CHECK( rig->calls.systems == std::vector<SystemAddress>{ kExpiredSystem } );
    CHECK( rig->filter.GetSystemFilterSet( kExpiredSystem ) == -1 );
    CHECK( rig->filter.GetSystemFilterSet( kLiveSystem ) == kLiveSetID );
    CHECK( rig->filter.GetSystemCount( -1 ) == 1 );
}

TEST_CASE( "MessageFilter's timeout callback may delete the expired System's filter set", "[messagefilter]" )
{
    // Deleting the set takes the other expired System out with it, so it gets no
    // callback of its own.
    auto rig = std::make_unique<UpdateRig>();
    rig->calls.deleteSetFrom = &rig->filter;
    ExpireSystems( *rig, { kExpiredSystem, kOtherExpiredSystem } );

    REQUIRE( UpdateReturns( rig ) );

    CHECK( rig->calls.systems.size() == 1 );
    CHECK( rig->filter.GetSystemFilterSet( kExpiredSystem ) == -1 );
    CHECK( rig->filter.GetSystemFilterSet( kOtherExpiredSystem ) == -1 );
    CHECK( rig->filter.GetSystemFilterSet( kLiveSystem ) == kLiveSetID );
    CHECK( rig->filter.GetFilterSetCount() == 1 );
}
