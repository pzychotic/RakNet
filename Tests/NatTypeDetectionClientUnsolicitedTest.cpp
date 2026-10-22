#include "Plugins/NatTypeDetectionClient.h"
#include "Plugins/NatTypeDetectionCommon.h"

#include "BitStream.h"
#include "ConnectionWaits.h"
#include "MessageIdentifiers.h"
#include "PeerScope.h"
#include "RakNetTypes.h"
#include "RakPeerInterface.h"
#include "ReliabilityLayerHarness.h"
#include "SocketDefines.h"

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <string>

/*
NatTypeDetectionClient acts on ID_NAT_TYPE_DETECTION_REQUEST and on a remote
ID_NAT_TYPE_DETECTION_RESULT only from the server passed to DetectNATType, while that
detection runs. From any other System both are consumed, whether or not detection runs:
the request sends nothing to the address it names, and the result neither ends detection
nor reaches Receive.

The server and the other System are plain peers that send the messages by hand, as
NatTypeDetectionServer writes them. The host a request names is a raw socket, which reads
whatever the client sends there.

After its forged messages the other System sends a marker on the same ordered channel.
Once the marker reaches Receive, the plugin has seen everything sent before it.
*/

using namespace RakNet;

namespace {

// Ports no other test uses.
constexpr unsigned short kServerPort = 31075;
constexpr unsigned short kIntruderPort = 31076;

constexpr MessageID kMarker = ID_USER_PACKET_ENUM;

// Hang guard. On loopback a message arrives a few update cycles after it is sent.
constexpr TimeMS kArrivalBudgetMs = 5000;

// How long the s3p4 socket is read for an answer that must not come. The client answers
// from inside OnReceive, so any answer is already on loopback once the marker arrives.
constexpr TimeMS kNoAnswerWaitMs = 300;

// NAT_TYPE_PORT_RESTRICTED and the client's GUID, as OnTestPortRestricted writes them.
constexpr size_t kAnswerBytes = sizeof( MessageID ) + sizeof( uint64_t );

void SendRequest( RakPeerInterface* sender, const SystemAddress& client, unsigned short s3p4Port )
{
    BitStream bs;
    bs.Write( (MessageID)ID_NAT_TYPE_DETECTION_REQUEST );
    bs.Write( std::string( "127.0.0.1" ) );
    bs.Write( s3p4Port );
    REQUIRE( sender->Send( &bs, HIGH_PRIORITY, RELIABLE_ORDERED, 0, client, false ) != 0 );
}

void SendResult( RakPeerInterface* sender, const SystemAddress& client, NATTypeDetectionResult result )
{
    BitStream bs;
    bs.Write( (MessageID)ID_NAT_TYPE_DETECTION_RESULT );
    bs.Write( (unsigned char)result );
    REQUIRE( sender->Send( &bs, HIGH_PRIORITY, RELIABLE_ORDERED, 0, client, false ) != 0 );
}

void SendMarker( RakPeerInterface* sender, const SystemAddress& client )
{
    BitStream bs;
    bs.Write( kMarker );
    REQUIRE( sender->Send( &bs, HIGH_PRIORITY, RELIABLE_ORDERED, 0, client, false ) != 0 );
}

// Whether an answer arrived within millisecondsToWait. Binding sends the socket four zero
// bytes from itself, which are no answer.
bool ReadAnswer( RNS2_Berkley& socket, int millisecondsToWait )
{
    timeval timeout;
    timeout.tv_sec = (long)( millisecondsToWait / 1000 );
    timeout.tv_usec = (long)( ( millisecondsToWait % 1000 ) * 1000 );

    fd_set readable;
    FD_ZERO( &readable );
    FD_SET( socket.GetSocket(), &readable );

    if( select__( (int)socket.GetSocket() + 1, &readable, 0, 0, &timeout ) <= 0 )
        return false;

    char data[64];
    sockaddr_storage from;
    socklen_t fromLength = sizeof( from );
    const int received = recvfrom__( socket.GetSocket(), data, sizeof( data ), 0, (sockaddr*)&from, &fromLength );
    return received == (int)kAnswerBytes && (unsigned char)data[0] == NAT_TYPE_PORT_RESTRICTED;
}

// Whether an answer arrived within budgetMs, skipping the bind datagram. Receives on client
// meanwhile, since it answers from inside Receive, and keeps the others drained.
bool WaitForAnswer( RNS2_Berkley& socket, RakPeerInterface* client, std::initializer_list<RakPeerInterface*> others, TimeMS budgetMs )
{
    return ConnectionWaits::WaitUntil(
        [&]() {
            ConnectionWaits::Drain( client );
            for( RakPeerInterface* other : others )
                ConnectionWaits::Drain( other );
            return ReadAnswer( socket, (int)ConnectionWaits::kPollInterval );
        },
        budgetMs );
}

// Receives on client until the marker arrives, counting the detection messages Receive
// hands out before it. Keeps the other peers drained meanwhile.
struct BeforeMarker
{
    bool sawMarker = false;
    int requests = 0;
    int results = 0;
};

BeforeMarker ReceiveUntilMarker( RakPeerInterface* client, std::initializer_list<RakPeerInterface*> others )
{
    BeforeMarker seen;
    ConnectionWaits::WaitUntil(
        [&]() {
            for( RakPeerInterface* other : others )
                ConnectionWaits::Drain( other );
            for( Packet* p = client->Receive(); p != nullptr; p = client->Receive() )
            {
                if( p->data[0] == kMarker )
                    seen.sawMarker = true;
                else if( !seen.sawMarker && p->data[0] == ID_NAT_TYPE_DETECTION_REQUEST )
                    ++seen.requests;
                else if( !seen.sawMarker && p->data[0] == ID_NAT_TYPE_DETECTION_RESULT )
                    ++seen.results;
                client->DeallocatePacket( p );
            }
            return seen.sawMarker;
        },
        kArrivalBudgetMs );
    return seen;
}

} // namespace

TEST_CASE( "NatTypeDetectionClient ignores another System's request and result during detection", "[nattypedetection][network]" )
{
    // Before the PeerScope, so it outlives the peers.
    NatTypeDetectionClient detection;

    PeerScope peers;
    RakPeerInterface* server = peers.Server( kServerPort );
    RakPeerInterface* intruder = peers.Server( kIntruderPort );
    RakPeerInterface* client = peers.Client( 0, 2 );
    client->AttachPlugin( &detection );

    ConnectionWaits::ConnectAndWait( client, server );
    ConnectionWaits::ConnectAndWait( client, intruder );
    detection.DetectNATType( SystemAddress( "127.0.0.1", kServerPort ) );

    ReliabilityLayerHarness::BoundSocket s3p4;
    const unsigned short s3p4Port = s3p4.Get().GetBoundAddress().GetPort();
    const SystemAddress clientAddress = ConnectionWaits::LoopbackAddressOf( client );

    SendRequest( intruder, clientAddress, s3p4Port );
    SendResult( intruder, clientAddress, NAT_TYPE_FULL_CONE );
    SendMarker( intruder, clientAddress );

    const BeforeMarker seen = ReceiveUntilMarker( client, { server, intruder } );
    REQUIRE( seen.sawMarker );
    CHECK( seen.results == 0 );
    CHECK( seen.requests == 0 );
    CHECK_FALSE( WaitForAnswer( s3p4.Get(), client, { server, intruder }, kNoAnswerWaitMs ) );

    // Detection still runs: the server's own request is answered, and its result ends
    // detection with the type the server sent.
    SendRequest( server, clientAddress, s3p4Port );
    CHECK( WaitForAnswer( s3p4.Get(), client, { server, intruder }, kArrivalBudgetMs ) );

    SendResult( server, clientAddress, NAT_TYPE_PORT_RESTRICTED );
    Packet* result = ConnectionWaits::ReceiveMessage( client, ID_NAT_TYPE_DETECTION_RESULT, kArrivalBudgetMs );
    REQUIRE( result != nullptr );
    CHECK( result->wasGeneratedLocally );
    CHECK( result->data[1] == NAT_TYPE_PORT_RESTRICTED );
    client->DeallocatePacket( result );

    client->DetachPlugin( &detection );
}

TEST_CASE( "NatTypeDetectionClient consumes a request and result while no detection runs", "[nattypedetection][network]" )
{
    // Before the PeerScope, so it outlives the peers.
    NatTypeDetectionClient detection;

    PeerScope peers;
    RakPeerInterface* server = peers.Server( kServerPort );
    RakPeerInterface* client = peers.Client();
    client->AttachPlugin( &detection );

    ConnectionWaits::ConnectAndWait( client, server );

    ReliabilityLayerHarness::BoundSocket s3p4;
    const unsigned short s3p4Port = s3p4.Get().GetBoundAddress().GetPort();
    const SystemAddress clientAddress = ConnectionWaits::LoopbackAddressOf( client );

    SendRequest( server, clientAddress, s3p4Port );
    SendResult( server, clientAddress, NAT_TYPE_FULL_CONE );
    SendMarker( server, clientAddress );

    const BeforeMarker seen = ReceiveUntilMarker( client, { server } );
    REQUIRE( seen.sawMarker );
    CHECK( seen.results == 0 );
    CHECK( seen.requests == 0 );
    CHECK_FALSE( WaitForAnswer( s3p4.Get(), client, { server }, kNoAnswerWaitMs ) );

    client->DetachPlugin( &detection );
}
