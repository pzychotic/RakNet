#include "Plugins/NatTypeDetectionClient.h"
#include "Plugins/NatTypeDetectionServer.h"
#include "Plugins/RelayPlugin.h"
#include "Plugins/TwoWayAuthentication.h"
#include "Plugins/UDPProxyClient.h"
#include "Plugins/UDPProxyCommon.h"
#include "Plugins/UDPProxyCoordinator.h"

#include "BitStream.h"
#include "ConnectionWaits.h"
#include "GetTime.h"
#include "InternalPacket.h"
#include "MarkerInjection.h"
#include "MessageIdentifiers.h"
#include "PeerScope.h"
#include "PluginInterface2.h"
#include "RakNetSocket2.h"
#include "RakNetStringMakers.h"
#include "RakNetTypes.h"
#include "RakPeerInterface.h"
#include "ReliabilityLayer.h"
#include "UDPProxyWire.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

/*
Pins what a connected System can make a plugin hold (ADR-0005).

- RelayPlugin. A repeated add request replaced the participant without leaving its group,
  so its copy stayed in the group and the group never emptied: add, join, add, join leaked
  one group per cycle, past the System's disconnect. Names and group names are capped. A
  message to a participant whose priority, reliability or channel is out of range is dropped,
  rather than sent on with a value that indexed the server's per-priority arrays past their end.
  A receipt reliability is forwarded without the receipt, so the server's application never
  gets ID_SND_RECEIPT_ACKED or ID_SND_RECEIPT_LOSS for a send it did not make. A group message
  is held to the same rules and forwarded with the sender's reliability and ordering channel.
- UDPProxyCoordinator. Forwarding requests are capped per requesting System, and so is the
  server selection data each keeps. Past either the request is refused with
  ID_UDP_PROXY_ALL_SERVERS_BUSY.
- UDPProxyClient. A ping-servers message listing no server made a group nothing reaped, and
  a short one claiming 65535 servers made 65535 entries. Groups are now one per coordinator,
  at most SetMaxServersPerPingGroup servers each, and go with the coordinator's connection.
- TwoWayAuthentication. A System could hold any number of nonces, and Update freed at most
  one stale nonce per call. A System now holds a few, the oldest evicted, and Update frees
  every nonce older than NONCE_TIMEOUT_MS.
- NatTypeDetectionServer and NatTypeDetectionClient. Any sender could grow the queue of
  datagrams to the plugin's sockets. It is capped at MAX_BUFFERED_RECEIVED_DATAGRAMS.

Router2's cap is pinned in Router2EntitlementTest.

Each injected Message is checked through MarkerInjection.
*/

using namespace RakNet;

namespace {

constexpr unsigned short kRelayPort = 31060;
constexpr unsigned short kCoordinatorPort = 31061;
constexpr unsigned short kProxyClientPort = 31062;
constexpr unsigned short kAuthPort = 31063;
// Nothing listens here.
constexpr unsigned short kSilentPort = 31069;

// Hang guard for a reply. On loopback it arrives a few update cycles after the send, tens of
// milliseconds.
constexpr TimeMS kStepBudgetMs = 5000;

// Receives on server and on peer until peer's Receive hands out a Message whose first two
// bytes are id and subId, and copies it into out. Anything else peer receives is dropped.
bool AwaitMessage( RakPeerInterface* server, RakPeerInterface* peer, MessageID id, MessageID subId, BitStream& out )
{
    const TimeMS deadline = GetTimeMS() + kStepBudgetMs;
    while( !ConnectionWaits::Expired( deadline ) )
    {
        ConnectionWaits::Drain( server );
        for( Packet* packet = peer->Receive(); packet != nullptr; packet = peer->Receive() )
        {
            const bool found = packet->length > 1 && packet->data[0] == id && packet->data[1] == subId;
            if( found )
                out.Write( (const char*)packet->data, packet->length );
            peer->DeallocatePacket( packet );
            if( found )
                return true;
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( ConnectionWaits::kPollInterval ) );
    }
    return false;
}

// Closes client's connection to server and receives on server until the plugins have heard.
void Disconnect( RakPeerInterface* server, unsigned short port, RakPeerInterface* client )
{
    const RakNetGUID clientGuid = client->GetMyGUID();
    client->CloseConnection( SystemAddress( "127.0.0.1", port ), true, 0, LOW_PRIORITY );
    // Plugins hear of a closed connection as Receive hands it up.
    const TimeMS deadline = GetTimeMS() + ConnectionWaits::kDisconnectBudget;
    bool closed = false;
    while( !closed )
    {
        REQUIRE( !ConnectionWaits::Expired( deadline ) );
        for( Packet* packet = server->Receive(); packet != nullptr; packet = server->Receive() )
        {
            closed |= ( packet->data[0] == ID_DISCONNECTION_NOTIFICATION || packet->data[0] == ID_CONNECTION_LOST ) && packet->guid == clientGuid;
            server->DeallocatePacket( packet );
        }
        if( !closed )
            std::this_thread::sleep_for( std::chrono::milliseconds( ConnectionWaits::kPollInterval ) );
    }
}

// ---------------------------------------------------------------------------------------------
// RelayPlugin

class RelayProbe : public RelayPlugin
{
public:
    size_t GroupCount() const { return chatRooms.size(); }
    bool IsParticipant( RakNetGUID guid ) const { return guidToStrHash.find( guid ) != guidToStrHash.end(); }
};

BitStream* RelayAdd( std::deque<BitStream>& storage, const std::string& name )
{
    storage.emplace_back();
    BitStream& bs = storage.back();
    bs.WriteCasted<MessageID>( ID_RELAY_PLUGIN );
    bs.WriteCasted<MessageID>( RPE_ADD_CLIENT_REQUEST_FROM_CLIENT );
    bs.WriteCompressed( name );
    return &bs;
}

BitStream* RelayJoin( std::deque<BitStream>& storage, const std::string& groupName )
{
    storage.emplace_back();
    BitStream& bs = storage.back();
    bs.WriteCasted<MessageID>( ID_RELAY_PLUGIN );
    bs.WriteCasted<MessageID>( RPE_JOIN_GROUP_REQUEST_FROM_CLIENT );
    bs.WriteCompressed( groupName );
    return &bs;
}

// RPE_MESSAGE_TO_SERVER_FROM_CLIENT for the participant named key, laid out as
// RelayPlugin::SendToParticipant writes it, with the forwarded copy's parameters as raw bytes.
// The payload is the single byte tag.
BitStream* RelayToParticipant( std::deque<BitStream>& storage, const std::string& key, unsigned char priority, unsigned char reliability,
                               unsigned char orderingChannel, unsigned char tag )
{
    BitStream payload;
    payload.Write( tag );
    storage.emplace_back();
    BitStream& bs = storage.back();
    bs.WriteCasted<MessageID>( ID_RELAY_PLUGIN );
    bs.WriteCasted<MessageID>( RPE_MESSAGE_TO_SERVER_FROM_CLIENT );
    bs.Write( priority );
    bs.Write( reliability );
    bs.Write( orderingChannel );
    bs.WriteCompressed( key );
    bs.Write( &payload );
    return &bs;
}

// RPE_GROUP_MESSAGE_FROM_CLIENT, laid out as RelayPlugin::SendGroupMessage writes it, with the
// forwarded copies' parameters as raw bytes. The payload is the single byte tag.
BitStream* RelayGroupMessage( std::deque<BitStream>& storage, unsigned char priority, unsigned char reliability, unsigned char orderingChannel,
                              unsigned char tag )
{
    BitStream payload;
    payload.Write( tag );
    storage.emplace_back();
    BitStream& bs = storage.back();
    bs.WriteCasted<MessageID>( ID_RELAY_PLUGIN );
    bs.WriteCasted<MessageID>( RPE_GROUP_MESSAGE_FROM_CLIENT );
    bs.Write( priority );
    bs.Write( reliability );
    bs.Write( orderingChannel );
    bs.Write( &payload );
    return &bs;
}

// Adds member as a participant named name and joins it to groupName.
void JoinRelayGroup( RakPeerInterface* server, RelayProbe& relay, RakPeerInterface* member, std::deque<BitStream>& storage,
                     const std::string& name, const std::string& groupName )
{
    MarkerInjection::Inject( member, server, std::vector<BitStream*>{ RelayAdd( storage, name ), RelayJoin( storage, groupName ) } );
    REQUIRE( relay.IsParticipant( member->GetMyGUID() ) );
}

// Records the reliability and ordering channel of every RPE_GROUP_MSG_FROM_SERVER its peer
// receives, as the reliability layer hands it up.
class GroupMessageProbe : public PluginInterface2
{
public:
    bool UsesReliabilityLayer( void ) const override { return true; }

    void OnInternalPacket( InternalPacket* internalPacket, unsigned frameNumber, SystemAddress remoteSystemAddress, RakNet::TimeMS time, int isSend ) override
    {
        (void)frameNumber;
        (void)remoteSystemAddress;
        (void)time;
        if( isSend || internalPacket->dataBitLength < 16 || internalPacket->data[0] != ID_RELAY_PLUGIN ||
            internalPacket->data[1] != RPE_GROUP_MSG_FROM_SERVER )
            return;
        std::lock_guard<std::mutex> guard( m_mutex );
        m_seen.emplace_back( internalPacket->reliability, internalPacket->orderingChannel );
    }

    std::vector<std::pair<PacketReliability, unsigned char>> Seen()
    {
        std::lock_guard<std::mutex> guard( m_mutex );
        return m_seen;
    }

private:
    std::mutex m_mutex;
    std::vector<std::pair<PacketReliability, unsigned char>> m_seen;
};

// A started client with probe attached. A plugin that uses the reliability layer is attached
// before Startup.
RakPeerInterface* ProbedClient( PeerScope& peers, GroupMessageProbe& probe )
{
    RakPeerInterface* client = peers.Create();
    client->AttachPlugin( &probe );
    SocketDescriptor socketDescriptor( 0, 0 );
    REQUIRE( client->Startup( 1, &socketDescriptor, 1 ) == RAKNET_STARTED );
    return client;
}

// How long a forwarded copy that should not exist gets to show up after the one that should.
constexpr TimeMS kStragglerWaitMs = 250;

// Receives on server and on target until target has been forwarded a copy tagged until, then
// for kStragglerWaitMs more, and returns the tag of every copy target was forwarded. A
// forwarded copy is a relay Message whose second byte is subId. If serverIds is given, the
// first byte of every Message server receives meanwhile goes into it.
std::vector<unsigned char> ForwardedTags( RakPeerInterface* server, RakPeerInterface* target, MessageID subId, unsigned char until,
                                          std::vector<MessageID>* serverIds = nullptr )
{
    std::vector<unsigned char> tags;
    TimeMS deadline = GetTimeMS() + kStepBudgetMs;
    bool seen = false;
    while( !ConnectionWaits::Expired( deadline ) )
    {
        for( Packet* packet = server->Receive(); packet != nullptr; packet = server->Receive() )
        {
            if( serverIds != nullptr && packet->length > 0 )
                serverIds->push_back( packet->data[0] );
            server->DeallocatePacket( packet );
        }
        for( Packet* packet = target->Receive(); packet != nullptr; packet = target->Receive() )
        {
            if( packet->length > 2 && packet->data[0] == ID_RELAY_PLUGIN && packet->data[1] == subId )
            {
                BitStream in( packet->data, packet->length, false );
                in.IgnoreBytes( sizeof( MessageID ) * 2 );
                std::string sender;
                in.ReadCompressed( sender );
                in.AlignReadToByteBoundary();
                unsigned char tag = 0;
                in.Read( tag );
                tags.push_back( tag );
                if( tag == until && !seen )
                {
                    seen = true;
                    deadline = GetTimeMS() + kStragglerWaitMs;
                }
            }
            target->DeallocatePacket( packet );
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( ConnectionWaits::kPollInterval ) );
    }
    REQUIRE( seen );
    return tags;
}

// ---------------------------------------------------------------------------------------------
// UDPProxyCoordinator

class CoordinatorProbe : public UDPProxyCoordinator
{
public:
    unsigned int RequestsFrom( const SystemAddress& requester ) const { return CountRequestsFrom( requester ); }
};

const char* const kProxyPassword = "password";

// Neither is connected to the coordinator; a request by address allows that.
SystemAddress UnconnectedTarget( unsigned int index )
{
    return SystemAddress( ( "10.0.1." + std::to_string( index + 1 ) ).c_str(), 2002 );
}

// ---------------------------------------------------------------------------------------------
// UDPProxyClient

// ID_UDP_PROXY_PING_SERVERS_FROM_COORDINATOR_TO_CLIENT naming claimedCount servers and
// listing listedCount of them, all the silent port on loopback.
void WritePingServers( BitStream& bs, unsigned short claimedCount, unsigned int listedCount )
{
    bs.Write( (MessageID)ID_UDP_PROXY_GENERAL );
    bs.Write( (MessageID)ID_UDP_PROXY_PING_SERVERS_FROM_COORDINATOR_TO_CLIENT );
    bs.Write( UNASSIGNED_SYSTEM_ADDRESS );
    bs.Write( SystemAddress( "10.0.1.2", 2002 ) );
    bs.Write( RakNetGUID( 2001 ) );
    bs.Write( claimedCount );
    for( unsigned int i = 0; i < listedCount; i++ )
        bs.Write( SystemAddress( "127.0.0.1", kSilentPort ) );
}

// ---------------------------------------------------------------------------------------------
// TwoWayAuthentication

// Mirrors the unexported NegotiationIdentifiers in TwoWayAuthentication.cpp.
constexpr MessageID kNonceRequest = 0;

class AuthProbe : public TwoWayAuthentication
{
public:
    std::vector<unsigned short> NonceIdsFor( RakNetGUID guid ) const
    {
        std::vector<unsigned short> ids;
        for( const NonceAndRemoteSystemRequest* nonce : nonceGenerator.generatedNonces )
        {
            if( nonce->remoteSystem.rakNetGuid == guid )
                ids.push_back( nonce->requestId );
        }
        return ids;
    }
    size_t NonceCount() const { return nonceGenerator.generatedNonces.size(); }
    Time OldestNonceTime() const { return nonceGenerator.generatedNonces.front()->whenGenerated; }
    Time NewestNonceTime() const { return nonceGenerator.generatedNonces.back()->whenGenerated; }
    void ExpireNonces( Time now ) { nonceGenerator.Update( now ); }
};

BitStream* NonceRequest( std::deque<BitStream>& storage )
{
    storage.emplace_back();
    BitStream& bs = storage.back();
    bs.Write( (MessageID)ID_TWO_WAY_AUTHENTICATION_NEGOTIATION );
    bs.Write( kNonceRequest );
    return &bs;
}

// ---------------------------------------------------------------------------------------------
// NatTypeDetection

template<class Plugin>
class NatQueueProbe : public Plugin
{
public:
    size_t Buffered()
    {
        std::lock_guard<std::mutex> guard( this->bufferedPacketsMutex );
        return this->bufferedPackets.size();
    }
};

// Hands plugin count datagrams, as its sockets' receive threads would.
template<class Plugin>
void Deliver( Plugin& plugin, unsigned int count )
{
    for( unsigned int i = 0; i < count; i++ )
    {
        RNS2RecvStruct* s = plugin.AllocRNS2RecvStruct( _FILE_AND_LINE_ );
        s->bytesRead = 1;
        s->data[0] = (char)0xFF;
        s->socket = nullptr;
        plugin.OnRNS2Recv( s );
    }
}

} // namespace

TEST_CASE( "RelayPlugin holds one group per participant", "[relay][network]" )
{
    // Before the PeerScope, so it outlives the peers.
    RelayProbe relay;
    relay.SetAcceptAddParticipantRequests( true );

    PeerScope peers;
    RakPeerInterface* server = peers.Server( kRelayPort, 4 );
    RakPeerInterface* looper = peers.Client();
    RakPeerInterface* other = peers.Client();
    server->AttachPlugin( &relay );
    ConnectionWaits::ConnectAndWait( looper, server );
    ConnectionWaits::ConnectAndWait( other, server );

    std::deque<BitStream> storage;

    SECTION( "An add, join, add, join loop leaves no group behind after disconnect" )
    {
        std::vector<BitStream*> loop;
        for( int i = 0; i < 10; i++ )
        {
            loop.push_back( RelayAdd( storage, "looper" + std::to_string( i ) ) );
            loop.push_back( RelayJoin( storage, "group" + std::to_string( i ) ) );
        }
        MarkerInjection::Inject( looper, server, loop );
        CHECK( relay.GroupCount() == 1 );

        // The plugin still serves another System
        std::vector<BitStream*> join{ RelayAdd( storage, "other" ), RelayJoin( storage, "shared" ) };
        MarkerInjection::Inject( other, server, join );
        BitStream reply;
        REQUIRE( AwaitMessage( server, other, ID_RELAY_PLUGIN, RPE_JOIN_GROUP_SUCCESS, reply ) );
        CHECK( relay.GroupCount() == 2 );

        Disconnect( server, kRelayPort, looper );
        CHECK( relay.GroupCount() == 1 );
        CHECK( !relay.IsParticipant( looper->GetMyGUID() ) );
    }

    SECTION( "A name over the cap is refused with RPE_ADD_CLIENT_NOT_ALLOWED" )
    {
        CHECK( relay.GetMaxNameLength() == 256 );

        MarkerInjection::Inject( looper, server, *RelayAdd( storage, std::string( 257, 'n' ) ) );
        BitStream refused;
        REQUIRE( AwaitMessage( server, looper, ID_RELAY_PLUGIN, RPE_ADD_CLIENT_NOT_ALLOWED, refused ) );
        CHECK( !relay.IsParticipant( looper->GetMyGUID() ) );
        CHECK( relay.GetNamesRefused() == 1 );

        MarkerInjection::Inject( looper, server, *RelayAdd( storage, std::string( 256, 'n' ) ) );
        BitStream added;
        REQUIRE( AwaitMessage( server, looper, ID_RELAY_PLUGIN, RPE_ADD_CLIENT_SUCCESS, added ) );
        CHECK( relay.IsParticipant( looper->GetMyGUID() ) );
    }

    SECTION( "A group name over the cap is refused with RPE_JOIN_GROUP_FAILURE" )
    {
        relay.SetMaxNameLength( 8 );
        CHECK( relay.GetMaxNameLength() == 8 );

        std::vector<BitStream*> join{ RelayAdd( storage, "looper" ), RelayJoin( storage, "ninechars" ) };
        MarkerInjection::Inject( looper, server, join );
        BitStream refused;
        REQUIRE( AwaitMessage( server, looper, ID_RELAY_PLUGIN, RPE_JOIN_GROUP_FAILURE, refused ) );
        CHECK( relay.GroupCount() == 0 );
        CHECK( relay.GetNamesRefused() == 1 );

        MarkerInjection::Inject( looper, server, *RelayJoin( storage, "eightchr" ) );
        BitStream joined;
        REQUIRE( AwaitMessage( server, looper, ID_RELAY_PLUGIN, RPE_JOIN_GROUP_SUCCESS, joined ) );
        CHECK( relay.GroupCount() == 1 );
    }

    server->DetachPlugin( &relay );
}

TEST_CASE( "RelayPlugin drops a message to a participant whose send parameters are out of range", "[relay][network]" )
{
    // Before the PeerScope, so it outlives the peers.
    RelayProbe relay;
    relay.SetAcceptAddParticipantRequests( true );

    PeerScope peers;
    RakPeerInterface* server = peers.Server( kRelayPort, 4 );
    RakPeerInterface* sender = peers.Client();
    RakPeerInterface* target = peers.Client();
    server->AttachPlugin( &relay );
    ConnectionWaits::ConnectAndWait( sender, server );
    ConnectionWaits::ConnectAndWait( target, server );

    std::deque<BitStream> storage;
    MarkerInjection::Inject( sender, server, *RelayAdd( storage, "sender" ) );
    MarkerInjection::Inject( target, server, *RelayAdd( storage, "target" ) );
    REQUIRE( relay.IsParticipant( sender->GetMyGUID() ) );
    REQUIRE( relay.IsParticipant( target->GetMyGUID() ) );

    constexpr unsigned char kFirst = 1;
    constexpr unsigned char kSecond = 2;
    // The highest valid values. RELIABLE_ORDERED_WITH_ACK_RECEIPT is the highest reliability.
    constexpr unsigned char kTopPriority = NUMBER_OF_PRIORITIES - 1;
    constexpr unsigned char kTopReliability = NUMBER_OF_RELIABILITIES - 1;
    constexpr unsigned char kTopChannel = NUMBER_OF_ORDERED_STREAMS - 1;

    unsigned char priority = kTopPriority;
    unsigned char reliability = kTopReliability;
    unsigned char channel = kTopChannel;
    SECTION( "priority NUMBER_OF_PRIORITIES" ) { priority = NUMBER_OF_PRIORITIES; }
    SECTION( "reliability NUMBER_OF_RELIABILITIES" ) { reliability = NUMBER_OF_RELIABILITIES; }
    SECTION( "channel NUMBER_OF_ORDERED_STREAMS" ) { channel = NUMBER_OF_ORDERED_STREAMS; }
    SECTION( "channel 128, a negative char" ) { channel = 128; }
    SECTION( "the highest valid values still arrive" ) {}

    // The second, always valid, bounds the wait for the first.
    std::vector<BitStream*> messages{ RelayToParticipant( storage, "target", priority, reliability, channel, kFirst ),
                                      RelayToParticipant( storage, "target", kTopPriority, kTopReliability, kTopChannel, kSecond ) };
    MarkerInjection::Inject( sender, server, messages );

    const bool allValid = priority == kTopPriority && reliability == kTopReliability && channel == kTopChannel;
    const std::vector<unsigned char> expected = allValid ? std::vector<unsigned char>{ kFirst, kSecond } : std::vector<unsigned char>{ kSecond };
    CHECK( ForwardedTags( server, target, RPE_MESSAGE_TO_CLIENT_FROM_SERVER, kSecond ) == expected );

    server->DetachPlugin( &relay );
}

TEST_CASE( "RelayPlugin forwards a message relayed with a receipt reliability without the receipt", "[relay][network]" )
{
    // Before the PeerScope, so it outlives the peers.
    RelayProbe relay;
    relay.SetAcceptAddParticipantRequests( true );

    PeerScope peers;
    RakPeerInterface* server = peers.Server( kRelayPort, 4 );
    RakPeerInterface* sender = peers.Client();
    RakPeerInterface* target = peers.Client();
    server->AttachPlugin( &relay );
    ConnectionWaits::ConnectAndWait( sender, server );
    ConnectionWaits::ConnectAndWait( target, server );

    std::deque<BitStream> storage;
    MarkerInjection::Inject( sender, server, *RelayAdd( storage, "sender" ) );
    MarkerInjection::Inject( target, server, *RelayAdd( storage, "target" ) );
    REQUIRE( relay.IsParticipant( sender->GetMyGUID() ) );
    REQUIRE( relay.IsParticipant( target->GetMyGUID() ) );

    PacketReliability reliability = UNRELIABLE_WITH_ACK_RECEIPT;
    SECTION( "UNRELIABLE_WITH_ACK_RECEIPT" ) { reliability = UNRELIABLE_WITH_ACK_RECEIPT; }
    SECTION( "RELIABLE_WITH_ACK_RECEIPT" ) { reliability = RELIABLE_WITH_ACK_RECEIPT; }
    SECTION( "RELIABLE_ORDERED_WITH_ACK_RECEIPT" ) { reliability = RELIABLE_ORDERED_WITH_ACK_RECEIPT; }

    constexpr unsigned char kTag = 1;
    MarkerInjection::Inject( sender, server, *RelayToParticipant( storage, "target", HIGH_PRIORITY, (unsigned char)reliability, 0, kTag ) );

    std::vector<MessageID> serverIds;
    CHECK( ForwardedTags( server, target, RPE_MESSAGE_TO_CLIENT_FROM_SERVER, kTag, &serverIds ) == std::vector<unsigned char>{ kTag } );
    for( MessageID id : serverIds )
    {
        CHECK( id != ID_SND_RECEIPT_ACKED );
        CHECK( id != ID_SND_RECEIPT_LOSS );
    }

    server->DetachPlugin( &relay );
}

TEST_CASE( "RelayPlugin forwards a group message with the sender's reliability and ordering channel", "[relay][network]" )
{
    // Before the PeerScope, so they outlive the peers.
    RelayProbe relay;
    relay.SetAcceptAddParticipantRequests( true );
    GroupMessageProbe firstProbe;
    GroupMessageProbe secondProbe;

    PeerScope peers;
    RakPeerInterface* server = peers.Server( kRelayPort, 4 );
    RakPeerInterface* sender = peers.Client();
    RakPeerInterface* first = ProbedClient( peers, firstProbe );
    RakPeerInterface* second = ProbedClient( peers, secondProbe );
    server->AttachPlugin( &relay );

    std::deque<BitStream> storage;
    for( auto [member, name] : { std::pair{ sender, "sender" }, std::pair{ first, "first" }, std::pair{ second, "second" } } )
    {
        ConnectionWaits::ConnectAndWait( member, server );
        JoinRelayGroup( server, relay, member, storage, name, "group" );
    }

    constexpr unsigned char kTag = 1;
    constexpr unsigned char kChannel = 3;
    MarkerInjection::Inject( sender, server, *RelayGroupMessage( storage, HIGH_PRIORITY, UNRELIABLE_SEQUENCED, kChannel, kTag ) );

    // Priority does not cross the wire.
    const std::vector<std::pair<PacketReliability, unsigned char>> expected{ { UNRELIABLE_SEQUENCED, kChannel } };
    CHECK( ForwardedTags( server, first, RPE_GROUP_MSG_FROM_SERVER, kTag ) == std::vector<unsigned char>{ kTag } );
    CHECK( firstProbe.Seen() == expected );
    CHECK( ForwardedTags( server, second, RPE_GROUP_MSG_FROM_SERVER, kTag ) == std::vector<unsigned char>{ kTag } );
    CHECK( secondProbe.Seen() == expected );

    server->DetachPlugin( &relay );
}

TEST_CASE( "RelayPlugin drops a group message whose send parameters are out of range", "[relay][network]" )
{
    // Before the PeerScope, so it outlives the peers.
    RelayProbe relay;
    relay.SetAcceptAddParticipantRequests( true );

    PeerScope peers;
    RakPeerInterface* server = peers.Server( kRelayPort, 4 );
    RakPeerInterface* sender = peers.Client();
    RakPeerInterface* target = peers.Client();
    server->AttachPlugin( &relay );
    ConnectionWaits::ConnectAndWait( sender, server );
    ConnectionWaits::ConnectAndWait( target, server );

    std::deque<BitStream> storage;
    JoinRelayGroup( server, relay, sender, storage, "sender", "group" );
    JoinRelayGroup( server, relay, target, storage, "target", "group" );

    constexpr unsigned char kFirst = 1;
    constexpr unsigned char kSecond = 2;
    // The highest valid values. RELIABLE_ORDERED_WITH_ACK_RECEIPT is the highest reliability.
    constexpr unsigned char kTopPriority = NUMBER_OF_PRIORITIES - 1;
    constexpr unsigned char kTopReliability = NUMBER_OF_RELIABILITIES - 1;
    constexpr unsigned char kTopChannel = NUMBER_OF_ORDERED_STREAMS - 1;

    unsigned char priority = kTopPriority;
    unsigned char reliability = kTopReliability;
    unsigned char channel = kTopChannel;
    SECTION( "priority NUMBER_OF_PRIORITIES" ) { priority = NUMBER_OF_PRIORITIES; }
    SECTION( "reliability NUMBER_OF_RELIABILITIES" ) { reliability = NUMBER_OF_RELIABILITIES; }
    SECTION( "channel NUMBER_OF_ORDERED_STREAMS" ) { channel = NUMBER_OF_ORDERED_STREAMS; }
    SECTION( "channel 128, a negative char" ) { channel = 128; }
    SECTION( "the highest valid values still arrive" ) {}

    // The second, always valid, bounds the wait for the first.
    std::vector<BitStream*> messages{ RelayGroupMessage( storage, priority, reliability, channel, kFirst ),
                                      RelayGroupMessage( storage, kTopPriority, kTopReliability, kTopChannel, kSecond ) };
    MarkerInjection::Inject( sender, server, messages );

    const bool allValid = priority == kTopPriority && reliability == kTopReliability && channel == kTopChannel;
    const std::vector<unsigned char> expected = allValid ? std::vector<unsigned char>{ kFirst, kSecond } : std::vector<unsigned char>{ kSecond };
    CHECK( ForwardedTags( server, target, RPE_GROUP_MSG_FROM_SERVER, kSecond ) == expected );

    server->DetachPlugin( &relay );
}

TEST_CASE( "RelayPlugin forwards a group message sent with a receipt reliability without the receipt", "[relay][network]" )
{
    // Before the PeerScope, so it outlives the peers.
    RelayProbe relay;
    relay.SetAcceptAddParticipantRequests( true );

    PeerScope peers;
    RakPeerInterface* server = peers.Server( kRelayPort, 4 );
    RakPeerInterface* sender = peers.Client();
    RakPeerInterface* first = peers.Client();
    RakPeerInterface* second = peers.Client();
    server->AttachPlugin( &relay );

    std::deque<BitStream> storage;
    for( auto [member, name] : { std::pair{ sender, "sender" }, std::pair{ first, "first" }, std::pair{ second, "second" } } )
    {
        ConnectionWaits::ConnectAndWait( member, server );
        JoinRelayGroup( server, relay, member, storage, name, "group" );
    }

    PacketReliability reliability = UNRELIABLE_WITH_ACK_RECEIPT;
    SECTION( "UNRELIABLE_WITH_ACK_RECEIPT" ) { reliability = UNRELIABLE_WITH_ACK_RECEIPT; }
    SECTION( "RELIABLE_WITH_ACK_RECEIPT" ) { reliability = RELIABLE_WITH_ACK_RECEIPT; }
    SECTION( "RELIABLE_ORDERED_WITH_ACK_RECEIPT" ) { reliability = RELIABLE_ORDERED_WITH_ACK_RECEIPT; }

    constexpr unsigned char kTag = 1;
    MarkerInjection::Inject( sender, server, *RelayGroupMessage( storage, HIGH_PRIORITY, (unsigned char)reliability, 0, kTag ) );

    std::vector<MessageID> serverIds;
    CHECK( ForwardedTags( server, first, RPE_GROUP_MSG_FROM_SERVER, kTag, &serverIds ) == std::vector<unsigned char>{ kTag } );
    CHECK( ForwardedTags( server, second, RPE_GROUP_MSG_FROM_SERVER, kTag, &serverIds ) == std::vector<unsigned char>{ kTag } );
    for( MessageID id : serverIds )
    {
        CHECK( id != ID_SND_RECEIPT_ACKED );
        CHECK( id != ID_SND_RECEIPT_LOSS );
    }

    server->DetachPlugin( &relay );
}

TEST_CASE( "UDPProxyCoordinator refuses a System's requests past its cap", "[udpproxy][network]" )
{
    // Before the PeerScope, so it outlives the peers.
    CoordinatorProbe coordinatorPlugin;
    coordinatorPlugin.SetRemoteLoginPassword( kProxyPassword );

    PeerScope peers;
    RakPeerInterface* coordinator = peers.Server( kCoordinatorPort, 4 );
    RakPeerInterface* proxyServer = peers.Client();
    RakPeerInterface* requester = peers.Client();
    RakPeerInterface* other = peers.Client();
    coordinator->AttachPlugin( &coordinatorPlugin );
    for( RakPeerInterface* peer : { proxyServer, requester, other } )
        ConnectionWaits::ConnectAndWait( peer, coordinator );
    const SystemAddress requesterAddress = coordinator->GetSystemAddressFromGuid( requester->GetMyGUID() );
    const SystemAddress otherAddress = coordinator->GetSystemAddressFromGuid( other->GetMyGUID() );

    // One proxy server, which never answers, so every accepted request stays open.
    BitStream login;
    login.Write( (MessageID)ID_UDP_PROXY_GENERAL );
    login.Write( (MessageID)ID_UDP_PROXY_LOGIN_REQUEST_FROM_SERVER_TO_COORDINATOR );
    login.Write( std::string( kProxyPassword ) );
    MarkerInjection::Inject( proxyServer, coordinator, login );

    CHECK( coordinatorPlugin.GetMaxForwardingRequestsPerSystem() == 8 );
    CHECK( coordinatorPlugin.GetMaxServerSelectionBitstreamBytes() == 1024 );

    SECTION( "A System's requests past the cap are answered all servers busy" )
    {
        std::deque<BitStream> requests( 10 );
        std::vector<BitStream*> flood;
        for( unsigned int i = 0; i < requests.size(); i++ )
        {
            UDPProxyWire::WriteForwardingRequest( requests[i], UNASSIGNED_SYSTEM_ADDRESS, UnconnectedTarget( i ) );
            flood.push_back( &requests[i] );
        }
        MarkerInjection::Inject( requester, coordinator, flood );

        CHECK( coordinatorPlugin.RequestsFrom( requesterAddress ) == 8 );
        CHECK( coordinatorPlugin.GetForwardingRequestsRefused() == 2 );

        BitStream busy;
        REQUIRE( AwaitMessage( coordinator, requester, ID_UDP_PROXY_GENERAL, ID_UDP_PROXY_ALL_SERVERS_BUSY, busy ) );
        busy.IgnoreBytes( 2 );
        SystemAddress source, target;
        REQUIRE( busy.Read( source ) );
        REQUIRE( busy.Read( target ) );
        CHECK( target == UnconnectedTarget( 8 ) );

        // The plugin still serves another System
        BitStream otherRequest;
        UDPProxyWire::WriteForwardingRequest( otherRequest, UNASSIGNED_SYSTEM_ADDRESS, UnconnectedTarget( 20 ) );
        MarkerInjection::Inject( other, coordinator, otherRequest );
        CHECK( coordinatorPlugin.RequestsFrom( otherAddress ) == 1 );

        Disconnect( coordinator, kCoordinatorPort, requester );
        CHECK( coordinatorPlugin.RequestsFrom( requesterAddress ) == 0 );
        CHECK( coordinatorPlugin.RequestsFrom( otherAddress ) == 1 );
    }

    SECTION( "Server selection data over the cap is answered all servers busy" )
    {
        BitStream tooLong;
        UDPProxyWire::WriteForwardingRequest( tooLong, UNASSIGNED_SYSTEM_ADDRESS, UnconnectedTarget( 0 ), 1025 );
        MarkerInjection::Inject( requester, coordinator, tooLong );
        BitStream busy;
        REQUIRE( AwaitMessage( coordinator, requester, ID_UDP_PROXY_GENERAL, ID_UDP_PROXY_ALL_SERVERS_BUSY, busy ) );
        CHECK( coordinatorPlugin.RequestsFrom( requesterAddress ) == 0 );
        CHECK( coordinatorPlugin.GetServerSelectionBitstreamsRefused() == 1 );

        BitStream atCap;
        UDPProxyWire::WriteForwardingRequest( atCap, UNASSIGNED_SYSTEM_ADDRESS, UnconnectedTarget( 1 ), 1024 );
        MarkerInjection::Inject( requester, coordinator, atCap );
        CHECK( coordinatorPlugin.RequestsFrom( requesterAddress ) == 1 );
        CHECK( coordinatorPlugin.GetServerSelectionBitstreamsRefused() == 1 );
    }

    coordinator->DetachPlugin( &coordinatorPlugin );
}

TEST_CASE( "UDPProxyClient holds one bounded ping group per coordinator", "[udpproxy][network]" )
{
    // Before the PeerScope, so it outlives the peers.
    UDPProxyClient proxyClient;

    PeerScope peers;
    RakPeerInterface* client = peers.Server( kProxyClientPort, 2 );
    RakPeerInterface* coordinator = peers.Client();
    RakPeerInterface* otherCoordinator = peers.Client();
    client->AttachPlugin( &proxyClient );
    ConnectionWaits::ConnectAndWait( coordinator, client );
    ConnectionWaits::ConnectAndWait( otherCoordinator, client );
    const SystemAddress coordinatorAddress = client->GetSystemAddressFromGuid( coordinator->GetMyGUID() );
    proxyClient.AddCoordinator( coordinatorAddress );
    proxyClient.AddCoordinator( client->GetSystemAddressFromGuid( otherCoordinator->GetMyGUID() ) );

    CHECK( proxyClient.GetMaxServersPerPingGroup() == 64 );

    SECTION( "A message listing no server leaves nothing behind" )
    {
        BitStream empty;
        WritePingServers( empty, 0, 0 );
        MarkerInjection::Inject( coordinator, client, empty );
        CHECK( proxyClient.pingServerGroups.empty() );
    }

    SECTION( "A short message pings only the servers it lists" )
    {
        BitStream shortMessage;
        WritePingServers( shortMessage, 65535, 2 );
        MarkerInjection::Inject( coordinator, client, shortMessage );
        REQUIRE( proxyClient.pingServerGroups.size() == 1 );
        CHECK( proxyClient.pingServerGroups.front()->serversToPing.size() == 2 );
    }

    SECTION( "A message over the cap pings only the first servers" )
    {
        BitStream oversized;
        WritePingServers( oversized, 100, 100 );
        MarkerInjection::Inject( coordinator, client, oversized );
        REQUIRE( proxyClient.pingServerGroups.size() == 1 );
        CHECK( proxyClient.pingServerGroups.front()->serversToPing.size() == 64 );
        CHECK( proxyClient.GetPingServersTruncated() == 1 );
    }

    SECTION( "A coordinator looping the message holds one group, and another coordinator keeps its own" )
    {
        std::deque<BitStream> messages( 20 );
        std::vector<BitStream*> flood;
        for( BitStream& message : messages )
        {
            WritePingServers( message, 3, 3 );
            flood.push_back( &message );
        }
        MarkerInjection::Inject( coordinator, client, flood );
        CHECK( proxyClient.pingServerGroups.size() == 1 );
        CHECK( proxyClient.GetPingServerGroupsReplaced() == 19 );

        BitStream fromOther;
        WritePingServers( fromOther, 2, 2 );
        MarkerInjection::Inject( otherCoordinator, client, fromOther );
        CHECK( proxyClient.pingServerGroups.size() == 2 );

        // A group goes with its coordinator's connection
        Disconnect( client, kProxyClientPort, coordinator );
        REQUIRE( proxyClient.pingServerGroups.size() == 1 );
        CHECK( proxyClient.pingServerGroups.front()->coordinatorAddressForPings != coordinatorAddress );
    }

    client->DetachPlugin( &proxyClient );
}

TEST_CASE( "TwoWayAuthentication caps the nonces a System holds", "[twowayauth][network]" )
{
    // Before the PeerScope, so they outlive the peers.
    AuthProbe auth;
    TwoWayAuthentication challengerAuth;
    REQUIRE( auth.AddPassword( "identifier", "password" ) );
    REQUIRE( challengerAuth.AddPassword( "identifier", "password" ) );

    PeerScope peers;
    RakPeerInterface* server = peers.Server( kAuthPort, 2 );
    RakPeerInterface* flooder = peers.Client();
    RakPeerInterface* challenger = peers.Client();
    server->AttachPlugin( &auth );
    challenger->AttachPlugin( &challengerAuth );
    ConnectionWaits::ConnectAndWait( flooder, server );
    ConnectionWaits::ConnectAndWait( challenger, server );
    const RakNetGUID flooderGuid = flooder->GetMyGUID();

    CHECK( auth.GetMaxNoncesPerSystem() == 4 );

    std::deque<BitStream> storage;

    SECTION( "A System past the cap loses its oldest nonce" )
    {
        std::vector<BitStream*> flood;
        for( int i = 0; i < 6; i++ )
            flood.push_back( NonceRequest( storage ) );
        MarkerInjection::Inject( flooder, server, flood );

        CHECK( auth.NonceIdsFor( flooderGuid ) == std::vector<unsigned short>{ 2, 3, 4, 5 } );
        CHECK( auth.GetNoncesEvicted() == 2 );
    }

    SECTION( "A System still authenticates while another floods nonce requests" )
    {
        REQUIRE( challengerAuth.Challenge( "identifier", server->GetMyGUID() ) );

        // The server's plugin runs inside MarkerInjection::Inject's Receive. It sends the
        // challenger a success only once the challenger's hash matched the nonce the server
        // kept for it.
        bool challengerPassed = false;
        const TimeMS deadline = GetTimeMS() + kStepBudgetMs;
        while( !challengerPassed && !ConnectionWaits::Expired( deadline ) )
        {
            std::vector<BitStream*> flood;
            for( int i = 0; i < 8; i++ )
                flood.push_back( NonceRequest( storage ) );
            MarkerInjection::Inject( flooder, server, flood );
            CHECK( auth.NonceIdsFor( flooderGuid ).size() <= 4 );
            storage.clear();

            for( Packet* packet = challenger->Receive(); packet != nullptr; packet = challenger->Receive() )
            {
                challengerPassed |= packet->data[0] == ID_TWO_WAY_AUTHENTICATION_OUTGOING_CHALLENGE_SUCCESS;
                challenger->DeallocatePacket( packet );
            }
            ConnectionWaits::Drain( flooder );
        }
        CHECK( challengerPassed );
        CHECK( auth.GetNoncesEvicted() > 0 );
    }

    SECTION( "Update frees every nonce older than NONCE_TIMEOUT_MS, and only those" )
    {
        std::vector<BitStream*> requests{ NonceRequest( storage ), NonceRequest( storage ), NonceRequest( storage ) };
        MarkerInjection::Inject( flooder, server, requests );
        REQUIRE( auth.NonceCount() == 3 );
        const Time oldest = auth.OldestNonceTime();
        const Time newest = auth.NewestNonceTime();

        // The nonces may be stamped in different milliseconds, so each bound is taken from the one it spares or frees last
        auth.ExpireNonces( oldest + NONCE_TIMEOUT_MS );
        CHECK( auth.NonceCount() == 3 );

        auth.ExpireNonces( newest + NONCE_TIMEOUT_MS + 1 );
        CHECK( auth.NonceCount() == 0 );
    }

    challenger->DetachPlugin( &challengerAuth );
    server->DetachPlugin( &auth );
}

TEST_CASE( "NatTypeDetection plugins drop datagrams past MAX_BUFFERED_RECEIVED_DATAGRAMS", "[nattypedetection]" )
{
    SECTION( "NatTypeDetectionServer" )
    {
        NatQueueProbe<NatTypeDetectionServer> plugin;
        Deliver( plugin, MAX_BUFFERED_RECEIVED_DATAGRAMS + 3 );
        CHECK( plugin.Buffered() == MAX_BUFFERED_RECEIVED_DATAGRAMS );
        CHECK( plugin.GetReceivedDatagramsDroppedAtCap() == 3 );
    }

    SECTION( "NatTypeDetectionClient" )
    {
        NatQueueProbe<NatTypeDetectionClient> plugin;
        Deliver( plugin, MAX_BUFFERED_RECEIVED_DATAGRAMS + 3 );
        CHECK( plugin.Buffered() == MAX_BUFFERED_RECEIVED_DATAGRAMS );
        CHECK( plugin.GetReceivedDatagramsDroppedAtCap() == 3 );
    }
}
