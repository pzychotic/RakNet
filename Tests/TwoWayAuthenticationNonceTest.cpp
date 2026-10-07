#include "Plugins/TwoWayAuthentication.h"

#include "BitStream.h"
#include "ConnectionWaits.h"
#include "MarkerInjection.h"
#include "MessageIdentifiers.h"
#include "PeerScope.h"
#include "RakNetTypes.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstring>
#include <set>
#include <string>
#include <vector>

/*
TwoWayAuthentication's nonces are the challenge a Peer issues, so they must be
unpredictable (ADR-0001): they come from the platform CSPRNG and nothing else.

The source is NonceGenerator::fillRandomBytes. With the real CSPRNG behind it the
failure path is unreachable, so the failing sources below stand in for it. A nonce
that could not be drawn is never stored and never sent; the challenger times out.

The wire case is checked through MarkerInjection: the server answers a nonce request on
channel 0, RELIABLE_ORDERED, so a marker it sends afterwards reaches the requester after
any reply.
*/

using namespace RakNet;

namespace {

constexpr unsigned short kAuthPort = 31070;
// TwoWayAuthentication.cpp's NegotiationIdentifiers
constexpr MessageID kNonceRequest = 0;

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
    ConnectionWaits::ConnectAndWait( requester, server );

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
    MarkerInjection::Inject( requester, server, request );
    // A marker alone, back the other way, so any reply comes out ahead of it.
    const std::vector<MessageID> received = MarkerInjection::Inject( server, requester, {} );

    // The requester runs no plugin, so the only negotiation Message the server sends it is the
    // nonce reply.
    CHECK( std::count( received.begin(), received.end(), ID_TWO_WAY_AUTHENTICATION_NEGOTIATION ) == ( drawFails ? 0 : 1 ) );
    CHECK( auth.NonceCount() == ( drawFails ? 0u : 1u ) );

    server->DetachPlugin( &auth );
}
