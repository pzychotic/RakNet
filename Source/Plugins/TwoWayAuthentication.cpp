/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "NativeFeatureIncludes.h"
#if _RAKNET_SUPPORT_TwoWayAuthentication == 1

#include "Plugins/TwoWayAuthentication.h"
#include "Rand.h"
#include "GetTime.h"
#include "MessageIdentifiers.h"
#include "BitStream.h"
#include "RakPeerInterface.h"

#if LIBCAT_SECURITY == 1
static const int HASH_BITS = 256;
static const int HASH_BYTES = HASH_BITS / 8;
static const int STRENGTHENING_FACTOR = 256;
#include <cat/crypt/hash/Skein.hpp>
#endif

namespace RakNet {

enum NegotiationIdentifiers
{
    ID_NONCE_REQUEST,
    ID_NONCE_REPLY,
    ID_HASHED_NONCE_AND_PASSWORD,
};

TwoWayAuthentication::NonceGenerator::NonceGenerator() { nextRequestId = 0; }
TwoWayAuthentication::NonceGenerator::~NonceGenerator()
{
    Clear();
}
void TwoWayAuthentication::NonceGenerator::GetNonce( char nonce[TWO_WAY_AUTHENTICATION_NONCE_LENGTH], unsigned short* requestId, AddressOrGUID remoteSystem )
{
    TwoWayAuthentication::NonceAndRemoteSystemRequest* narsr = RakNet::OP_NEW<TwoWayAuthentication::NonceAndRemoteSystemRequest>( _FILE_AND_LINE_ );
    narsr->remoteSystem = remoteSystem;
    GenerateNonce( narsr->nonce );
    narsr->requestId = nextRequestId++;
    *requestId = narsr->requestId;
    memcpy( nonce, narsr->nonce, TWO_WAY_AUTHENTICATION_NONCE_LENGTH );
    narsr->whenGenerated = RakNet::GetTime();
    generatedNonces.push_back( narsr );
}
void TwoWayAuthentication::NonceGenerator::GenerateNonce( char nonce[TWO_WAY_AUTHENTICATION_NONCE_LENGTH] )
{
    fillBufferMT( nonce, TWO_WAY_AUTHENTICATION_NONCE_LENGTH );
}
bool TwoWayAuthentication::NonceGenerator::GetNonceById( char nonce[TWO_WAY_AUTHENTICATION_NONCE_LENGTH], unsigned short requestId, AddressOrGUID remoteSystem, bool popIfFound )
{
    for( auto it = generatedNonces.begin(); it != generatedNonces.end(); ++it )
    {
        TwoWayAuthentication::NonceAndRemoteSystemRequest* pNonce = *it;
        if( pNonce->requestId == requestId )
        {
            if( remoteSystem == pNonce->remoteSystem )
            {
                memcpy( nonce, pNonce->nonce, TWO_WAY_AUTHENTICATION_NONCE_LENGTH );
                if( popIfFound )
                {
                    RakNet::OP_DELETE( pNonce, _FILE_AND_LINE_ );
                    generatedNonces.erase( it );
                }
                return true;
            }
            else
            {
                return false;
            }
        }
    }
    return false;
}
void TwoWayAuthentication::NonceGenerator::Clear( void )
{
    for( TwoWayAuthentication::NonceAndRemoteSystemRequest* pNonce : generatedNonces )
    {
        RakNet::OP_DELETE( pNonce, _FILE_AND_LINE_ );
    }
    generatedNonces.clear();
}
void TwoWayAuthentication::NonceGenerator::ClearByAddress( AddressOrGUID remoteSystem )
{
    for( auto it = generatedNonces.begin(); it != generatedNonces.end(); /**/ )
    {
        TwoWayAuthentication::NonceAndRemoteSystemRequest* pNonce = *it;
        if( pNonce->remoteSystem == remoteSystem )
        {
            RakNet::OP_DELETE( pNonce, _FILE_AND_LINE_ );
            it = generatedNonces.erase( it );
        }
        else
        {
            ++it;
        }
    }
}
void TwoWayAuthentication::NonceGenerator::Update( RakNet::Time curTime )
{
    if( !generatedNonces.empty() && GreaterThan( curTime - 5000, generatedNonces[0]->whenGenerated ) )
    {
        RakNet::OP_DELETE( generatedNonces[0], _FILE_AND_LINE_ );
        generatedNonces.erase( generatedNonces.begin() );
    }
}
TwoWayAuthentication::TwoWayAuthentication()
{
    whenLastTimeoutCheck = RakNet::GetTime();
    seedMT( RakNet::GetTimeMS() );
}
TwoWayAuthentication::~TwoWayAuthentication()
{
    Clear();
}

bool TwoWayAuthentication::AddPassword( const std::string& identifier, const std::string& password )
{
    if( password.empty() || identifier.empty() || password == identifier /* insecure */ )
        return false;

    if( passwords.find( identifier ) != passwords.end() )
        return false; // identifier already in use

    passwords.insert( std::make_pair( identifier, password ) );
    return true;
}

bool TwoWayAuthentication::Challenge( const std::string& identifier, AddressOrGUID remoteSystem )
{
    if( passwords.find( identifier ) == passwords.end() )
        return false;

    BitStream bsOut;
    bsOut.Write( (MessageID)ID_TWO_WAY_AUTHENTICATION_NEGOTIATION );
    bsOut.Write( (MessageID)ID_NONCE_REQUEST );
    SendUnified( &bsOut, HIGH_PRIORITY, RELIABLE_ORDERED, 0, remoteSystem, false );

    PendingChallenge pc;
    pc.identifier = identifier;
    pc.remoteSystem = remoteSystem;
    pc.time = RakNet::GetTime();
    pc.sentHash = false;
    outgoingChallenges.push_back( pc );

    return true;
}

void TwoWayAuthentication::Update( void )
{
    RakNet::Time curTime = RakNet::GetTime();
    nonceGenerator.Update( curTime );
    if( GreaterThan( curTime - CHALLENGE_MINIMUM_TIMEOUT, whenLastTimeoutCheck ) )
    {
        while( !outgoingChallenges.empty() && GreaterThan( curTime - CHALLENGE_MINIMUM_TIMEOUT, outgoingChallenges.front().time ) )
        {
            PendingChallenge pc = outgoingChallenges.front();
            outgoingChallenges.pop_front();

            // Tell the user about the timeout
            PushToUser( ID_TWO_WAY_AUTHENTICATION_OUTGOING_CHALLENGE_TIMEOUT, pc.identifier, pc.remoteSystem );
        }

        whenLastTimeoutCheck = curTime + CHALLENGE_MINIMUM_TIMEOUT;
    }
}
PluginReceiveResult TwoWayAuthentication::OnReceive( Packet* packet )
{
    switch( packet->data[0] )
    {
    case ID_TWO_WAY_AUTHENTICATION_NEGOTIATION: {
        if( packet->length >= sizeof( MessageID ) * 2 )
        {
            switch( packet->data[sizeof( MessageID )] )
            {
            case ID_NONCE_REQUEST: {
                OnNonceRequest( packet );
            }
            break;
            case ID_NONCE_REPLY: {
                OnNonceReply( packet );
            }
            break;
            case ID_HASHED_NONCE_AND_PASSWORD: {
                return OnHashedNonceAndPassword( packet );
            }
            break;
            }
        }
        return RR_STOP_PROCESSING_AND_DEALLOCATE;
    }
    case ID_TWO_WAY_AUTHENTICATION_OUTGOING_CHALLENGE_FAILURE:
    case ID_TWO_WAY_AUTHENTICATION_OUTGOING_CHALLENGE_SUCCESS: {
        if( packet->wasGeneratedLocally == false )
        {
            OnPasswordResult( packet );
            return RR_STOP_PROCESSING_AND_DEALLOCATE;
        }
        else
            break;
    }
    break;
    // These should only be generated locally
    case ID_TWO_WAY_AUTHENTICATION_INCOMING_CHALLENGE_SUCCESS:
    case ID_TWO_WAY_AUTHENTICATION_INCOMING_CHALLENGE_FAILURE:
    case ID_TWO_WAY_AUTHENTICATION_OUTGOING_CHALLENGE_TIMEOUT:
        if( packet->wasGeneratedLocally == false )
            return RR_STOP_PROCESSING_AND_DEALLOCATE;
        break;
    }

    return RR_CONTINUE_PROCESSING;
}
void TwoWayAuthentication::OnRakPeerShutdown( void )
{
    Clear();
}
void TwoWayAuthentication::OnClosedConnection( const SystemAddress& systemAddress, RakNetGUID rakNetGUID, PI2_LostConnectionReason lostConnectionReason )
{
    (void)lostConnectionReason;

    // Remove from pending challenges
    outgoingChallenges.erase( outgoingChallenges .end());
    for( auto it = outgoingChallenges.begin(); it != outgoingChallenges.end(); )
    {
        if( ( rakNetGUID != UNASSIGNED_RAKNET_GUID && (*it).remoteSystem.rakNetGuid == rakNetGUID ) ||
            ( systemAddress != UNASSIGNED_SYSTEM_ADDRESS && (*it).remoteSystem.systemAddress == systemAddress ) )
        {
            it = outgoingChallenges.erase( it );
        }
        else
        {
            ++it;
        }
    }

    if( rakNetGUID != UNASSIGNED_RAKNET_GUID )
        nonceGenerator.ClearByAddress( rakNetGUID );
    else
        nonceGenerator.ClearByAddress( systemAddress );
}

void TwoWayAuthentication::Clear( void )
{
    outgoingChallenges.clear();
    passwords.clear();
    nonceGenerator.Clear();
}

void TwoWayAuthentication::PushToUser( MessageID messageId, const std::string& password, AddressOrGUID remoteSystem )
{
    BitStream output;
    output.Write( messageId );
    if( password.empty() == false )
        output.Write( password );
    Packet* p = AllocatePacketUnified( output.GetNumberOfBytesUsed() );
    p->systemAddress = remoteSystem.systemAddress;
    p->systemAddress.systemIndex = (SystemIndex)-1;
    p->guid = remoteSystem.rakNetGuid;
    p->wasGeneratedLocally = true;
    memcpy( p->data, output.GetData(), output.GetNumberOfBytesUsed() );
    rakPeerInterface->PushBackPacket( p, true );
}
void TwoWayAuthentication::OnNonceRequest( Packet* packet )
{
    BitStream bsIn( packet->data, packet->length, false );
    bsIn.IgnoreBytes( sizeof( MessageID ) * 2 );

    char nonce[TWO_WAY_AUTHENTICATION_NONCE_LENGTH];
    unsigned short requestId;
    nonceGenerator.GetNonce( nonce, &requestId, packet );

    BitStream bsOut;
    bsOut.Write( (MessageID)ID_TWO_WAY_AUTHENTICATION_NEGOTIATION );
    bsOut.Write( (MessageID)ID_NONCE_REPLY );
    bsOut.Write( requestId );
    bsOut.WriteAlignedBytes( (const unsigned char*)nonce, TWO_WAY_AUTHENTICATION_NONCE_LENGTH );
    SendUnified( &bsOut, HIGH_PRIORITY, RELIABLE_ORDERED, 0, packet, false );
}

void TwoWayAuthentication::OnNonceReply( Packet* packet )
{
    BitStream bsIn( packet->data, packet->length, false );
    bsIn.IgnoreBytes( sizeof( MessageID ) * 2 );

    char thierNonce[TWO_WAY_AUTHENTICATION_NONCE_LENGTH];
    unsigned short requestId;
    bsIn.Read( requestId );
    bsIn.ReadAlignedBytes( (unsigned char*)thierNonce, TWO_WAY_AUTHENTICATION_NONCE_LENGTH );

    // Lookup one of the negotiations for this guid/system address
    AddressOrGUID aog( packet );
    for( PendingChallenge& rChallenge : outgoingChallenges )
    {
        if( rChallenge.remoteSystem == aog && rChallenge.sentHash == false )
        {
            rChallenge.sentHash = true;

            // Get the password for this identifier
            if( const auto it = passwords.find( rChallenge.identifier ); it != passwords.end() )
            {
                // Hash their nonce with password and reply
                char hashedNonceAndPw[HASHED_NONCE_AND_PW_LENGTH];
                Hash( thierNonce, it->second, hashedNonceAndPw );

                // Send
                BitStream bsOut;
                bsOut.Write( (MessageID)ID_TWO_WAY_AUTHENTICATION_NEGOTIATION );
                bsOut.Write( (MessageID)ID_HASHED_NONCE_AND_PASSWORD );
                bsOut.Write( requestId );
                bsOut.Write( rChallenge.identifier ); // Identifier helps the other system lookup the password quickly.
                bsOut.WriteAlignedBytes( (const unsigned char*)hashedNonceAndPw, HASHED_NONCE_AND_PW_LENGTH );
                SendUnified( &bsOut, HIGH_PRIORITY, RELIABLE_ORDERED, 0, packet, false );
            }

            return;
        }
    }
}

PluginReceiveResult TwoWayAuthentication::OnHashedNonceAndPassword( Packet* packet )
{
    BitStream bsIn( packet->data, packet->length, false );
    bsIn.IgnoreBytes( sizeof( MessageID ) * 2 );

    char remoteHashedNonceAndPw[HASHED_NONCE_AND_PW_LENGTH];
    unsigned short requestId;
    bsIn.Read( requestId );
    std::string passwordIdentifier;
    bsIn.Read( passwordIdentifier );
    bsIn.ReadAlignedBytes( (unsigned char*)remoteHashedNonceAndPw, HASHED_NONCE_AND_PW_LENGTH );

    // Look up used nonce from requestId
    char usedNonce[TWO_WAY_AUTHENTICATION_NONCE_LENGTH];
    if( nonceGenerator.GetNonceById( usedNonce, requestId, packet, true ) == false )
        return RR_STOP_PROCESSING_AND_DEALLOCATE;

    if( const auto it = passwords.find( passwordIdentifier ); it != passwords.end() )
    {
        char hashedThisNonceAndPw[HASHED_NONCE_AND_PW_LENGTH];
        Hash( usedNonce, it->second, hashedThisNonceAndPw );
        if( memcmp( hashedThisNonceAndPw, remoteHashedNonceAndPw, HASHED_NONCE_AND_PW_LENGTH ) == 0 )
        {
            // Pass
            BitStream bsOut;
            bsOut.Write( (MessageID)ID_TWO_WAY_AUTHENTICATION_OUTGOING_CHALLENGE_SUCCESS );
            bsOut.WriteAlignedBytes( (const unsigned char*)usedNonce, TWO_WAY_AUTHENTICATION_NONCE_LENGTH );
            bsOut.WriteAlignedBytes( (const unsigned char*)remoteHashedNonceAndPw, HASHED_NONCE_AND_PW_LENGTH );
            bsOut.Write( passwordIdentifier );
            SendUnified( &bsOut, HIGH_PRIORITY, RELIABLE_ORDERED, 0, packet, false );

            // Incoming success, modify packet header to tell user
            PushToUser( ID_TWO_WAY_AUTHENTICATION_INCOMING_CHALLENGE_SUCCESS, passwordIdentifier, packet );

            return RR_STOP_PROCESSING_AND_DEALLOCATE;
        }
    }

    // Incoming failure, modify arrived packet header to tell user
    packet->data[0] = (MessageID)ID_TWO_WAY_AUTHENTICATION_INCOMING_CHALLENGE_FAILURE;

    BitStream bsOut;
    bsOut.Write( (MessageID)ID_TWO_WAY_AUTHENTICATION_OUTGOING_CHALLENGE_FAILURE );
    bsOut.WriteAlignedBytes( (const unsigned char*)usedNonce, TWO_WAY_AUTHENTICATION_NONCE_LENGTH );
    bsOut.WriteAlignedBytes( (const unsigned char*)remoteHashedNonceAndPw, HASHED_NONCE_AND_PW_LENGTH );
    bsOut.Write( passwordIdentifier );
    SendUnified( &bsOut, HIGH_PRIORITY, RELIABLE_ORDERED, 0, packet, false );

    return RR_CONTINUE_PROCESSING;
}

void TwoWayAuthentication::OnPasswordResult( Packet* packet )
{
    BitStream bsIn( packet->data, packet->length, false );
    bsIn.IgnoreBytes( sizeof( MessageID ) * 1 );
    char usedNonce[TWO_WAY_AUTHENTICATION_NONCE_LENGTH];
    bsIn.ReadAlignedBytes( (unsigned char*)usedNonce, TWO_WAY_AUTHENTICATION_NONCE_LENGTH );
    char hashedNonceAndPw[HASHED_NONCE_AND_PW_LENGTH];
    bsIn.ReadAlignedBytes( (unsigned char*)hashedNonceAndPw, HASHED_NONCE_AND_PW_LENGTH );
    std::string passwordIdentifier;
    bsIn.Read( passwordIdentifier );

    if( const auto it = passwords.find( passwordIdentifier ); it != passwords.end() )
    {
        char testHash[HASHED_NONCE_AND_PW_LENGTH];
        Hash( usedNonce, it->second, testHash );
        if( memcmp( testHash, hashedNonceAndPw, HASHED_NONCE_AND_PW_LENGTH ) == 0 )
        {
            // Lookup the outgoing challenge and remove it from the list
            AddressOrGUID aog( packet );
            for( auto it = outgoingChallenges.begin(); it != outgoingChallenges.end(); ++it )
            {
                if( (*it).identifier == passwordIdentifier &&
                    (*it).remoteSystem == aog &&
                    (*it).sentHash == true )
                {
                    outgoingChallenges.erase( it );

                    PushToUser( packet->data[0], passwordIdentifier, packet );
                    return;
                }
            }
        }
    }
}

void TwoWayAuthentication::Hash( char thierNonce[TWO_WAY_AUTHENTICATION_NONCE_LENGTH], const std::string& password, char out[HASHED_NONCE_AND_PW_LENGTH] )
{
#if LIBCAT_SECURITY == 1
    cat::Skein hash;
    if( !hash.BeginKey( HASH_BITS ) )
        return;
    hash.Crunch( thierNonce, TWO_WAY_AUTHENTICATION_NONCE_LENGTH );
    hash.Crunch( password.c_str(), (int)password.size() );
    hash.End();
    hash.Generate( out, HASH_BYTES, STRENGTHENING_FACTOR );
#else
    CSHA1 sha1;
    sha1.Update( (unsigned char*)thierNonce, TWO_WAY_AUTHENTICATION_NONCE_LENGTH );
    sha1.Update( (unsigned char*)password.c_str(), (uint32_t)password.size() );
    sha1.Final();
    sha1.GetHash( (unsigned char*)out );
#endif
}

} // namespace RakNet

#endif
