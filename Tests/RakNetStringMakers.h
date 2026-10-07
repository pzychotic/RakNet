#pragma once

#include "MessageIdentifiers.h"
#include "RakNetTypes.h"

#include <catch2/catch_tostring.hpp>

#include <string>

/*
 *  Teaches Catch2 to print RakNet's address and guid types.
 *
 *  Neither has an operator<<, so without this every
 *
 *      CHECK( client->GetSystemAddressFromIndex( 0 ) == serverAddress )
 *
 *  fails with "{?} == {?}". With it the same failure reads
 *  "127.0.0.1|30002 == 127.0.0.1|30000" and the bug is on the screen.
 *
 *  Tests/CMakeLists.txt force-includes this into every RakNetTests TU, and it has
 *  to: a specialization is the same symbol as the primary template's
 *  instantiation, so a single TU that compared a guid without it would emit a
 *  "{?}" convert under that name, and the linker keeps one copy for the whole
 *  binary. Including it explicitly as well is harmless.
 *
 *  Both types also expose a const char* ToString() returning a static buffer,
 *  documented NOT THREADSAFE; the char* dest overloads used here are the
 *  threadsafe ones, and copying into a std::string immediately means two of these
 *  in one expression cannot tread on each other.
 */
namespace Catch {

template<>
struct StringMaker<RakNet::SystemAddress>
{
    static std::string convert( const RakNet::SystemAddress& value )
    {
        char buffer[128] = { 0 };
        value.ToString( true, buffer );

        return std::string( buffer );
    }
};

template<>
struct StringMaker<RakNet::RakNetGUID>
{
    static std::string convert( const RakNet::RakNetGUID& value )
    {
        char buffer[128] = { 0 };
        value.ToString( buffer );

        return std::string( buffer );
    }
};

} // namespace Catch

/*
 *  ConnectionState is an enum with no names attached, so Catch2 would print it as
 *  its underlying number and a failed
 *
 *      REQUIRE( afterCancel == IS_NOT_CONNECTED )
 *
 *  would read "0 == 6". Registering it makes that "IS_PENDING ==
 *  IS_NOT_CONNECTED", which is the diagnosis rather than a lookup task.
 *  ConnectionStateName below reads the same table, for a state interpolated into
 *  a message of the test's own.
 *
 *  Must be at global scope: the macro opens `namespace Catch` itself.
 */
CATCH_REGISTER_ENUM( RakNet::ConnectionState, RakNet::IS_PENDING, RakNet::IS_CONNECTING, RakNet::IS_CONNECTED, RakNet::IS_DISCONNECTING, RakNet::IS_SILENTLY_DISCONNECTING, RakNet::IS_DISCONNECTED, RakNet::IS_NOT_CONNECTED )

// The registered name of state, for a FAIL, INFO or report the test composes itself:
// a stream insertion does not go through Catch2's stringification, so without this it
// would print the number.
inline std::string ConnectionStateName( RakNet::ConnectionState state )
{
    return Catch::StringMaker<RakNet::ConnectionState>::convert( state );
}

// The name of a Message ID that starts, refuses or ends a connection, and "ID <n>" for
// any other. MessageID is a byte and DefaultMessageIDTypes is not its type, so this is a
// function to call by name rather than a StringMaker.
inline std::string MessageIdName( RakNet::MessageID id )
{
    switch( id )
    {
    case RakNet::ID_CONNECTION_REQUEST_ACCEPTED:
        return "ID_CONNECTION_REQUEST_ACCEPTED";
    case RakNet::ID_NEW_INCOMING_CONNECTION:
        return "ID_NEW_INCOMING_CONNECTION";
    case RakNet::ID_CONNECTION_ATTEMPT_FAILED:
        return "ID_CONNECTION_ATTEMPT_FAILED";
    case RakNet::ID_ALREADY_CONNECTED:
        return "ID_ALREADY_CONNECTED";
    case RakNet::ID_NO_FREE_INCOMING_CONNECTIONS:
        return "ID_NO_FREE_INCOMING_CONNECTIONS";
    case RakNet::ID_CONNECTION_BANNED:
        return "ID_CONNECTION_BANNED";
    case RakNet::ID_INVALID_PASSWORD:
        return "ID_INVALID_PASSWORD";
    case RakNet::ID_INCOMPATIBLE_PROTOCOL_VERSION:
        return "ID_INCOMPATIBLE_PROTOCOL_VERSION";
    case RakNet::ID_IP_RECENTLY_CONNECTED:
        return "ID_IP_RECENTLY_CONNECTED";
    case RakNet::ID_DISCONNECTION_NOTIFICATION:
        return "ID_DISCONNECTION_NOTIFICATION";
    case RakNet::ID_CONNECTION_LOST:
        return "ID_CONNECTION_LOST";
    default:
        return "ID " + std::to_string( id );
    }
}
