#include "Plugins/TwoWayAuthentication.h"

#include "BitStream.h"
#include "ConnectionWaits.h"
#include "GetTime.h"
#include "MessageIdentifiers.h"
#include "PeerScope.h"
#include "RakNetTypes.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstring>
#include <set>
#include <string>
#include <thread>

/*
TwoWayAuthentication's nonces are the challenge a Peer issues, so they must be
unpredictable (ADR-0001): they come from the platform CSPRNG and nothing else.

The source is NonceGenerator::fillRandomBytes. With the real CSPRNG behind it the
failure path is unreachable, so the failing sources below stand in for it. A nonce
that could not be drawn is never stored and never sent; the challenger times out.

The wire case is ordered: the server answers a nonce request on channel 0,
RELIABLE_ORDERED, and the marker it sends afterwards goes the same way, so once the
marker reaches the requester any reply would have come out before it.
*/

using namespace RakNet;

namespace {

constexpr unsigned short kAuthPort = 31070;
constexpr TimeMS kStepBudgetMs = 5000;
constexpr MessageID kMarker = ID_USER_PACKET_ENUM;
// TwoWayAuthentication.cpp's NegotiationIdentifiers
constexpr MessageID kNonceRequest = 0;
constexpr MessageID kNonceReply = 1;

int fillCallCount = 0;

bool FailingSource( void*, size_t )
{
    fillCallCount++;
    return false;
}

// Writes half the buffer, then reports failure.
bool FailsPartwaySource( void* buffer, size_t bytes )
{
    fillCallCount++;
    memset( buffer, 0xAB, bytes / 2 );
    return false;
}

class NonceSourceProbe : public TwoWayAuthentication
{
public:
    void SetNonceSource( bool ( *source )( void*, size_t ) ) { nonceGenerator.fillRandomBytes = source; }
    size_t NonceCount() const { return nonceGenerator.generatedNonces.size(); }
};

void Connect( RakPeerInterface* server, RakPeerInterface* client )
{
    REQUIRE( client->Connect( "127.0.0.1", kAuthPort, nullptr, 0 ) == CONNECTION_ATTEMPT_STARTED );
    const SystemAddress serverAddress( "127.0.0.1", kAuthPort );
    const TimeMS deadline = GetTimeMS() + ConnectionWaits::kConnectionCountBudget;
    while( server->GetConnectionState( client->GetMyGUID() ) != IS_CONNECTED ||
           client->GetConnectionState( serverAddress ) != IS_CONNECTED )
    {
        REQUIRE( !ConnectionWaits::Expired( deadline ) );
        ConnectionWaits::Drain( server );
        ConnectionWaits::Drain( client );
        std::this_thread::sleep_for( std::chrono::milliseconds( ConnectionWaits::kPollInterval ) );
    }
    ConnectionWaits::Drain( client );
}

void SendMarker( RakPeerInterface* from, RakNetGUID to )
{
    BitStream marker;
    marker.Write( kMarker );
    from->Send( &marker, HIGH_PRIORITY, RELIABLE_ORDERED, 0, to, false );
}

// Receives on receiver, which runs its plugins, until the marker comes out. Counts the nonce
// replies that came out before it.
bool ReceiveUntilMarker( RakPeerInterface* receiver, int* nonceReplies )
{
    const TimeMS deadline = GetTimeMS() + kStepBudgetMs;
    while( !ConnectionWaits::Expired( deadline ) )
    {
        for( Packet* packet = receiver->Receive(); packet != nullptr; packet = receiver->Receive() )
        {
            const bool marker = packet->data[0] == kMarker;
            if( packet->data[0] == ID_TWO_WAY_AUTHENTICATION_NEGOTIATION && packet->length >= 2 && packet->data[1] == kNonceReply )
                ( *nonceReplies )++;
            receiver->DeallocatePacket( packet );
            if( marker )
                return true;
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( ConnectionWaits::kPollInterval ) );
    }
    return false;
}

} // namespace

TEST_CASE( "NonceGenerator draws distinct nonces from the platform CSPRNG", "[twowayauth]" )
{
    TwoWayAuthentication::NonceGenerator generator;
    std::set<std::string> seen;
    for( int i = 0; i < 100000; i++ )
    {
        char nonce[TWO_WAY_AUTHENTICATION_NONCE_LENGTH];
        REQUIRE( generator.GenerateNonce( nonce ) );
        seen.insert( std::string( nonce, TWO_WAY_AUTHENTICATION_NONCE_LENGTH ) );
    }
    CHECK( seen.size() == 100000 );
}

TEST_CASE( "NonceGenerator stores no nonce it could not draw", "[twowayauth]" )
{
    TwoWayAuthentication::NonceGenerator generator;
    const AddressOrGUID remoteSystem( RakNetGUID( 42 ) );
    char nonce[TWO_WAY_AUTHENTICATION_NONCE_LENGTH];
    unsigned short requestId = 0;
    fillCallCount = 0;

    SECTION( "A source that fails outright" )
    {
        generator.fillRandomBytes = &FailingSource;
        CHECK_FALSE( generator.GenerateNonce( nonce ) );
        CHECK_FALSE( generator.GetNonce( nonce, &requestId, remoteSystem ) );
    }
    SECTION( "A source that fails partway through a fill" )
    {
        generator.fillRandomBytes = &FailsPartwaySource;
        CHECK_FALSE( generator.GenerateNonce( nonce ) );
        CHECK_FALSE( generator.GetNonce( nonce, &requestId, remoteSystem ) );
    }

    CHECK( fillCallCount == 2 );
    CHECK( generator.generatedNonces.empty() );
    CHECK( generator.nextRequestId == 0 );
}

TEST_CASE( "NonceGenerator stores a nonce it drew", "[twowayauth]" )
{
    TwoWayAuthentication::NonceGenerator generator;
    const AddressOrGUID remoteSystem( RakNetGUID( 42 ) );
    char nonce[TWO_WAY_AUTHENTICATION_NONCE_LENGTH];
    unsigned short requestId = 0xFFFF;

    REQUIRE( generator.GetNonce( nonce, &requestId, remoteSystem ) );
    CHECK( requestId == 0 );
    CHECK( generator.nextRequestId == 1 );

    char stored[TWO_WAY_AUTHENTICATION_NONCE_LENGTH];
    REQUIRE( generator.GetNonceById( stored, requestId, remoteSystem, true ) );
    CHECK( memcmp( stored, nonce, TWO_WAY_AUTHENTICATION_NONCE_LENGTH ) == 0 );
}

TEST_CASE( "TwoWayAuthentication sends no nonce it could not draw", "[twowayauth][network]" )
{
    // Before the PeerScope, so it outlives the peers.
    NonceSourceProbe auth;

    PeerScope peers;
    RakPeerInterface* server = peers.Server( kAuthPort, 1 );
    RakPeerInterface* requester = peers.Client();
    server->AttachPlugin( &auth );
    Connect( server, requester );

    bool drawFails = false;
    SECTION( "A nonce it drew is sent" ) {}
    SECTION( "A nonce it could not draw is not" )
    {
        drawFails = true;
        auth.SetNonceSource( &FailingSource );
    }

    BitStream request;
    request.Write( (MessageID)ID_TWO_WAY_AUTHENTICATION_NEGOTIATION );
    request.Write( kNonceRequest );
    requester->Send( &request, HIGH_PRIORITY, RELIABLE_ORDERED, 0, server->GetMyGUID(), false );
    SendMarker( requester, server->GetMyGUID() );
    int ignored = 0;
    REQUIRE( ReceiveUntilMarker( server, &ignored ) );

    SendMarker( server, requester->GetMyGUID() );
    int nonceReplies = 0;
    REQUIRE( ReceiveUntilMarker( requester, &nonceReplies ) );

    CHECK( nonceReplies == ( drawFails ? 0 : 1 ) );
    CHECK( auth.NonceCount() == ( drawFails ? 0u : 1u ) );

    server->DetachPlugin( &auth );
}
