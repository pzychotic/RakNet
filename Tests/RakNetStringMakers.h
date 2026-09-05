#pragma once

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
 *  "127.0.0.1|60002 == 127.0.0.1|60000" and the bug is on the screen.
 *
 *  Include this in any test that compares these types.
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
 *  ConnectionWaits.cpp reads the same table by name, for the states it
 *  interpolates into FAIL messages of its own.
 *
 *  Must be at global scope: the macro opens `namespace Catch` itself.
 */
CATCH_REGISTER_ENUM( RakNet::ConnectionState, RakNet::IS_PENDING, RakNet::IS_CONNECTING, RakNet::IS_CONNECTED, RakNet::IS_DISCONNECTING, RakNet::IS_SILENTLY_DISCONNECTING, RakNet::IS_DISCONNECTED, RakNet::IS_NOT_CONNECTED )
