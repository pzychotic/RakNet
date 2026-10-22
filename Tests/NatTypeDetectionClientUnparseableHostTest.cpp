#include "Plugins/NatTypeDetectionClient.h"
#include "Plugins/NatTypeDetectionCommon.h"

#include "BitStream.h"
#include "ConnectionWaits.h"
#include "GetTime.h"
#include "MessageIdentifiers.h"
#include "PeerScope.h"
#include "RakNetTypes.h"
#include "RakPeerInterface.h"
#include "ReliabilityLayerHarness.h"
#include "SocketDefines.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

/*
NatTypeDetectionClient drops an ID_NAT_TYPE_DETECTION_REQUEST whose host doesn't parse, and
detection goes on: the server sends each request twice and decides from what comes back, so
one malformed request ends nothing. The client answers the next, valid, request as before.

The client used to send NAT_TYPE_PORT_RESTRICTED to the UNASSIGNED_SYSTEM_ADDRESS the failed
parse left. Nothing listening anywhere receives a datagram sent there, so this test can't
tell that send from no send. It pins the rest: a malformed request first neither ends
detection nor stops the client answering the valid one, and the client answers once.

The server is a plain peer that sends the two requests by hand, as NatTypeDetectionServer
writes them. The host they name is a raw socket, which reads the client's answer.

The host is numeric with a stray character after it, so the parse fails without a DNS lookup.
*/

using namespace RakNet;

namespace {

// Ports no other test uses.
constexpr unsigned short kServerPort = 31074;

const char* const kUnparseableHost = "127.0.0.1!";

// Hang guard. On loopback the answer arrives a few update cycles after the request.
constexpr TimeMS kAnswerBudgetMs = 5000;

// How long to go on reading once the answer is in, so a second one is read too.
constexpr TimeMS kSecondAnswerGraceMs = 500;

// NAT_TYPE_PORT_RESTRICTED and the client's GUID, as OnTestPortRestricted writes them.
constexpr size_t kAnswerBytes = sizeof( MessageID ) + sizeof( uint64_t );

void SendDetectionRequest( RakPeerInterface* server, const SystemAddress& client, const char* host, unsigned short port )
{
    BitStream bs;
    bs.Write( (MessageID)ID_NAT_TYPE_DETECTION_REQUEST );
    bs.Write( std::string( host ) );
    bs.Write( port );
    REQUIRE( server->Send( &bs, HIGH_PRIORITY, RELIABLE_ORDERED, 0, client, false ) != 0 );
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

} // namespace

TEST_CASE( "NatTypeDetectionClient drops a request whose host doesn't parse", "[nattypedetection][network]" )
{
    // Before the PeerScope, so it outlives the peers.
    NatTypeDetectionClient detection;

    PeerScope peers;
    RakPeerInterface* server = peers.Server( kServerPort );
    RakPeerInterface* client = peers.Client();
    client->AttachPlugin( &detection );

    ConnectionWaits::ConnectAndWait( client, server );
    detection.DetectNATType( SystemAddress( "127.0.0.1", kServerPort ) );

    ReliabilityLayerHarness::BoundSocket s3p4;
    const unsigned short s3p4Port = s3p4.Get().GetBoundAddress().GetPort();
    const SystemAddress clientAddress = ConnectionWaits::LoopbackAddressOf( client );

    SendDetectionRequest( server, clientAddress, kUnparseableHost, s3p4Port );
    SendDetectionRequest( server, clientAddress, "127.0.0.1", s3p4Port );

    int answers = 0;
    const TimeMS deadline = GetTimeMS() + kAnswerBudgetMs;
    TimeMS graceEnd = 0;
    while( !ConnectionWaits::Expired( answers == 0 ? deadline : graceEnd ) )
    {
        ConnectionWaits::Drain( server );
        ConnectionWaits::Drain( client );
        if( ReadAnswer( s3p4.Get(), (int)ConnectionWaits::kPollInterval ) )
        {
            if( answers++ == 0 )
                graceEnd = GetTimeMS() + kSecondAnswerGraceMs;
        }
    }

    CHECK( answers == 1 );

    client->DetachPlugin( &detection );
}
