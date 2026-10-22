/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

/// \file TwoWayAuthentication.h
/// \brief Implements two way authentication
/// \details Given two systems, each of whom knows a common password, one checks that the other knows it
/// by challenge and response. The password is not sent in plaintext, but the response lets the
/// remote System and any eavesdropper guess it offline; see the warning on TwoWayAuthentication.
/// This can be used to determine what permissions are should be allowed to the other system
///

#pragma once

#include "NativeFeatureIncludes.h"
#if _RAKNET_SUPPORT_TwoWayAuthentication == 1

// How long a nonce this Peer handed out stays valid. Update frees every older one.
#define NONCE_TIMEOUT_MS 10000
// How often to check for ID_TWO_WAY_AUTHENTICATION_OUTGOING_CHALLENGE_TIMEOUT, and the minimum timeout time. Maximum is double this value.
#define CHALLENGE_MINIMUM_TIMEOUT 3000

#include "DR_SHA1.h"
#define TWO_WAY_AUTHENTICATION_NONCE_LENGTH 20
#define HASHED_NONCE_AND_PW_LENGTH SHA1_LENGTH

#include "PluginInterface2.h"
#include "RakMemoryOverride.h"

#include <stdint.h>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace RakNet {

/// \brief Implements two way authentication
/// \details Given two systems, each of whom knows a common password / identifier pair, one checks that
/// the other knows the password by challenge and response, without sending it in plaintext.
/// This can be used to determine what permissions are should be allowed to the other system
/// If the other system should not send any data until authentication passes, you can use the MessageFilter plugin for this. Call MessageFilter::SetAllowMessageID() including ID_TWO_WAY_AUTHENTICATION_NEGOTIATION when doing so. Also attach MessageFilter first in the list of plugins
/// \note If another system challenges us and fails, you will get ID_TWO_WAY_AUTHENTICATION_INCOMING_CHALLENGE_FAILURE.
/// \warning The plugin has three limits:
/// - The password can be guessed offline. The response is SHA1( nonce || password ), with no salt
///   and no work factor, and the remote System picks the nonce. Any System this Peer challenges,
///   and any eavesdropper on an exchange, can test guesses at hash speed. A remote that always
///   sends the same nonce can use one precomputed table against every Peer it is challenged by.
/// - ID_TWO_WAY_AUTHENTICATION_OUTGOING_CHALLENGE_SUCCESS proves nothing about the remote System. The
///   reply echoes this Peer's own hash, so an impostor without the password can produce it. Only
///   ID_TWO_WAY_AUTHENTICATION_INCOMING_CHALLENGE_SUCCESS means the other side proved it knows the
///   password. For mutual authentication, both sides call Challenge(), and each trusts only its own
///   incoming result.
/// - A pass authenticates an address or GUID at one moment, not the traffic that follows. RakNet
///   has no transport security (ADR-0010).
/// Use high-entropy secrets, not human passwords. If any of these limits matters, run the plugin
/// inside a transport that is already encrypted and authenticated, such as DTLS or an encrypted
/// tunnel.
/// \ingroup PLUGINS_GROUP
class RAK_DLL_EXPORT TwoWayAuthentication : public PluginInterface2
{
public:
    // GetInstance() and DestroyInstance(instance*)
    STATIC_FACTORY_DECLARATIONS( TwoWayAuthentication )

    TwoWayAuthentication();
    ~TwoWayAuthentication() override;

    /// \brief Adds a password to the list of passwords the system will accept
    /// \details Each password, which is never sent in plaintext, is identified by \a identifier.
    /// \a identifier is transmitted in plaintext with the request. It is only needed because the system supports multiple password.
    /// It is used to only hash against once password on the remote system, rather than having to hash against every known password.
    /// \param[in] identifier A unique identifier representing this password. This is transmitted in plaintext and should be considered insecure
    /// \param[in] password The password to add
    /// \return True on success, false on identifier==password, either identifier or password is blank, or identifier is already in use
    [[nodiscard]] bool AddPassword( const std::string& identifier, const std::string& password );

    /// \brief Challenge another system for the specified identifier
    /// \details After calling Challenge, you will get back ID_TWO_WAY_AUTHENTICATION_OUTGOING_CHALLENGE_SUCCESS, ID_TWO_WAY_AUTHENTICATION_OUTGOING_CHALLENGE_TIMEOUT, or ID_TWO_WAY_AUTHENTICATION_OUTGOING_CHALLENGE_FAILURE
    /// The remote System sends ID_TWO_WAY_AUTHENTICATION_OUTGOING_CHALLENGE_SUCCESS when it accepts this Peer's response.
    /// \warning ID_TWO_WAY_AUTHENTICATION_OUTGOING_CHALLENGE_SUCCESS doesn't authenticate the remote System:
    /// an impostor without the password can produce it. To authenticate the remote System, it must call
    /// Challenge() in turn, and only this Peer's ID_TWO_WAY_AUTHENTICATION_INCOMING_CHALLENGE_SUCCESS counts.
    /// \param[in] identifier A unique identifier representing this password. This is transmitted in plaintext and should be considered insecure
    /// \return True on success, false on remote system not connected, or identifier not previously added with AddPassword()
    [[nodiscard]] bool Challenge( const std::string& identifier, AddressOrGUID remoteSystem );

    /// \brief Caps the nonces one remote System may hold at once.
    /// \details A System that challenges this Peer is handed a nonce, kept until it answers, it
    /// disconnects, or NONCE_TIMEOUT_MS passes. A request past the cap evicts that System's oldest
    /// nonce, counted in GetNoncesEvicted(), so a System that retries is never locked out; an answer
    /// to an evicted nonce fails.
    /// Defaults to 4. 0 is taken as 1, since a System must hold the nonce it answers.
    void SetMaxNoncesPerSystem( unsigned int max );

    /// \return The value passed to SetMaxNoncesPerSystem(), or the default.
    unsigned int GetMaxNoncesPerSystem( void ) const;

    /// \return How many nonces SetMaxNoncesPerSystem()'s cap has evicted.
    uint64_t GetNoncesEvicted( void ) const;

    /// \brief Free all memory
    void Clear( void );

    /// \internal
    void Update( void ) override;
    /// \internal
    PluginReceiveResult OnReceive( Packet* packet ) override;
    /// \internal
    void OnRakPeerShutdown( void ) override;
    /// \internal
    void OnClosedConnection( const SystemAddress& systemAddress, RakNetGUID rakNetGUID, PI2_LostConnectionReason lostConnectionReason ) override;

    /// \internal
    struct PendingChallenge
    {
        std::string identifier;
        AddressOrGUID remoteSystem;
        RakNet::Time time;
        bool sentHash;
    };

    std::deque<PendingChallenge> outgoingChallenges;

    /// \internal
    struct NonceAndRemoteSystemRequest
    {
        char nonce[TWO_WAY_AUTHENTICATION_NONCE_LENGTH];
        AddressOrGUID remoteSystem;
        unsigned short requestId;
        RakNet::Time whenGenerated;
    };
    /// \internal
    struct RAK_DLL_EXPORT NonceGenerator
    {
        NonceGenerator();
        ~NonceGenerator();
        NonceGenerator( const NonceGenerator& ) = delete;
        NonceGenerator& operator=( const NonceGenerator& ) = delete;
        /// Draws a nonce for \a remoteSystem and keeps it under a new request ID.
        /// \return false if no nonce could be drawn. Nothing is kept and no request ID is used.
        bool GetNonce( char nonce[TWO_WAY_AUTHENTICATION_NONCE_LENGTH], unsigned short* requestId, AddressOrGUID remoteSystem );
        /// Fills \a nonce from fillRandomBytes.
        /// \return false if the source failed, and then \a nonce must not be used.
        bool GenerateNonce( char nonce[TWO_WAY_AUTHENTICATION_NONCE_LENGTH] );
        bool GetNonceById( char nonce[TWO_WAY_AUTHENTICATION_NONCE_LENGTH], unsigned short requestId, AddressOrGUID remoteSystem, bool popIfFound );
        void Clear( void );
        void ClearByAddress( AddressOrGUID remoteSystem );
        void Update( RakNet::Time curTime );

        std::vector<std::unique_ptr<TwoWayAuthentication::NonceAndRemoteSystemRequest>> generatedNonces;
        unsigned short nextRequestId;
        unsigned int maxNoncesPerSystem;
        uint64_t noncesEvicted;
        /// Where nonces come from, with RakNet::FillRandomBytes' contract. Defaults to
        /// RakNet::FillRandomBytes, as a nonce must be unpredictable (ADR-0001). It exists only
        /// so tests can reach the failure path, which the real CSPRNG never takes.
        bool ( *fillRandomBytes )( void*, size_t );
        bool reportedDrawFailure;
    };

protected:
    void PushToUser( MessageID messageId, const std::string& password, AddressOrGUID remoteSystem );

    // Key is identifier, data is password
    std::unordered_map<std::string, std::string> passwords;

    RakNet::Time whenLastTimeoutCheck;

    NonceGenerator nonceGenerator;

    void OnNonceRequest( Packet* packet );
    void OnNonceReply( Packet* packet );
    PluginReceiveResult OnHashedNonceAndPassword( Packet* packet );
    void OnPasswordResult( Packet* packet );
    void Hash( char thierNonce[TWO_WAY_AUTHENTICATION_NONCE_LENGTH], const std::string& password, char out[HASHED_NONCE_AND_PW_LENGTH] );
};

} // namespace RakNet

#endif // _RAKNET_SUPPORT_*
