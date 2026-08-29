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
        if( value == RakNet::UNASSIGNED_SYSTEM_ADDRESS )
        {
            return "UNASSIGNED_SYSTEM_ADDRESS";
        }

        // Deliberately ToString( false, ... ) plus the port by hand, rather
        // than ToString( true, ... ). Both of SystemAddress's writePort=true
        // paths are broken: Source/RakNetTypes.cpp:276 calls
        // std::to_chars( dest, ... ), writing the port at the START of the
        // buffer and clobbering the IP it just wrote, so the address prints as
        // "60000" rather than "127.0.0.1|60000". The const char* overload
        // delegates to the same code and shares the bug. Routed around rather
        // than fixed, so this is correct whether or not the library ever is.
        char ip[128] = { 0 };
        value.ToString( false, ip );

        return std::string( ip ) + '|' + std::to_string( value.GetPort() );
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
