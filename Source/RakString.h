/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#pragma once

#include <cstdio>
#include "Export.h"
#include "DS_List.h"
#include "stdarg.h"

#ifdef _WIN32
#include "WindowsIncludes.h"
#endif

namespace std {
class mutex;
}

namespace RakNet {

/// Forward declarations
class BitStream;

/// \brief String class
/// \details Has the following improvements over std::string
/// -Reference counting: Suitable to store in lists
/// -Variadic assignment operator
/// -Doesn't cause linker errors
class RAK_DLL_EXPORT RakString
{
public:
    // Constructors
    RakString();
    RakString( char input );
    RakString( unsigned char input );
    RakString( const unsigned char* format, ... );
    RakString( const char* format, ... );
    ~RakString();
    RakString( const RakString& rhs );

    /// Same as std::string::c_str
    const char* C_String( void ) const { return sharedString->c_str; }

    /// Assigment operators
    RakString& operator=( const RakString& rhs );
    RakString& operator=( const char* str );
    RakString& operator=( char* str );
    RakString& operator=( const unsigned char* str );
    RakString& operator=( unsigned char* str );
    RakString& operator=( const char c );

    /// Concatenation
    RakString& operator+=( const RakString& rhs );
    RakString& operator+=( const char* str );
    RakString& operator+=( char* str );
    RakString& operator+=( const unsigned char* str );
    RakString& operator+=( unsigned char* str );
    RakString& operator+=( const char c );

    /// Character index. Do not use to change the string however.
    unsigned char operator[]( const unsigned int position ) const;

    /// Equality
    bool operator==( const RakString& rhs ) const;
    bool operator==( const char* str ) const;
    bool operator==( char* str ) const;

    // Comparison
    bool operator<( const RakString& right ) const;
    bool operator<=( const RakString& right ) const;
    bool operator>( const RakString& right ) const;
    bool operator>=( const RakString& right ) const;

    /// Inequality
    bool operator!=( const RakString& rhs ) const;
    bool operator!=( const char* str ) const;
    bool operator!=( char* str ) const;

    /// Set the value of the string
    void Set( const char* format, ... );

    /// Returns if the string is empty. Also, C_String() would return ""
    bool IsEmpty( void ) const;

    /// Returns the length of the string
    size_t GetLength( void ) const;

    /// Replace character at index with c
    void SetChar( unsigned index, unsigned char c );

    /// Make sure string is no longer than \a length
    void Truncate( unsigned int length );

    /// Erase characters out of the string at index for count
    void Erase( unsigned int index, unsigned int count );

    /// Create a RakString with a value, without doing printf style parsing
    /// Equivalent to assignment operator
    static RakString NonVariadic( const char* str );

    /// Hash the string into an unsigned int
    static unsigned long ToInteger( const RakString& rs );

    /// \brief Read an integer out of a substring
    /// \param[in] str The string
    /// \param[in] pos The position on str where the integer starts
    /// \param[in] n How many chars to copy
    static int ReadIntFromSubstring( const char* str, size_t pos, size_t n );

    // Like strncat, but for a fixed length
    void AppendBytes( const char* bytes, unsigned int count );

    /// Clear the string
    void Clear( void );

    /// URL Encode the string. See http://www.codeguru.com/cpp/cpp/cpp_mfc/article.php/c4029/
    RakString& URLEncode( void );

    /// URL decode the string
    RakString& URLDecode( void );

    /// https://servers.api.rackspacecloud.com/v1.0 to https://,  servers.api.rackspacecloud.com, /v1.0
    void SplitURI( RakString& header, RakString& domain, RakString& path );

    /// Format as a POST command that can be sent to a webserver
    /// \param[in] uri For example, masterserver2.raknet.com/testServer
    /// \param[in] contentType For example, text/plain; charset=UTF-8
    /// \param[in] body Body of the post
    /// \return Formatted string
    static RakString FormatForPOST( const char* uri, const char* contentType, const char* body, const char* extraHeaders = "" );
    static RakString FormatForPUT( const char* uri, const char* contentType, const char* body, const char* extraHeaders = "" );

    /// Format as a GET command that can be sent to a webserver
    /// \param[in] uri For example, masterserver2.raknet.com/testServer?__gameId=comprehensivePCGame
    /// \return Formatted string
    static RakString FormatForGET( const char* uri, const char* extraHeaders = "" );

    /// Format as a DELETE command that can be sent to a webserver
    /// \param[in] uri For example, masterserver2.raknet.com/testServer?__gameId=comprehensivePCGame&__rowId=1
    /// \return Formatted string
    static RakString FormatForDELETE( const char* uri, const char* extraHeaders = "" );

    /// RakString uses a freeList of old no-longer used strings
    /// Call this function to clear this memory on shutdown
    static void FreeMemory( void );
    /// \internal
    static void FreeMemoryNoMutex( void );

    /// Static version of the Serialize function
    static void Serialize( const char* str, BitStream* bs );

    /// Static version of the SerializeCompressed function
    static void SerializeCompressed( const char* str, BitStream* bs, uint8_t languageId = 0, bool writeLanguageId = false );

    /// Static version of the Deserialize() function
    static bool Deserialize( char* str, BitStream* bs );

    /// Static version of the DeserializeCompressed() function
    static bool DeserializeCompressed( char* str, BitStream* bs, bool readLanguageId = false );

    /// \internal
    static size_t GetSizeToAllocate( size_t bytes )
    {
        const size_t smallStringSize = 128 - sizeof( unsigned int ) - sizeof( size_t ) - sizeof( char* ) * 2;
        if( bytes <= smallStringSize )
            return smallStringSize;
        else
            return bytes * 2;
    }

    /// \internal
    struct SharedString
    {
        std::mutex* refCountMutex;
        unsigned int refCount;
        size_t bytesUsed;
        char* bigString;
        char* c_str;
        char smallString[128 - sizeof( unsigned int ) - sizeof( size_t ) - sizeof( char* ) * 2];
    };

    /// \internal
    RakString( SharedString* _sharedString );

    /// \internal
    SharedString* sharedString;

    /// \internal
    static SharedString emptyString;

    /// \internal
    /// List of free objects to reduce memory reallocations
    static DataStructures::List<SharedString*> freeList;

    static void LockMutex( void );
    static void UnlockMutex( void );

protected:
    static RakString FormatForPUTOrPost( const char* type, const char* uri, const char* contentType, const char* body, const char* extraHeaders );
    void Allocate( size_t len );
    void Assign( const char* str );
    void Assign( const char* str, va_list ap );

    void Clone( void );
    void Free( void );
    void Realloc( SharedString* sharedString, size_t bytes );
};

const RakString RAK_DLL_EXPORT operator+( const RakString& lhs, const RakString& rhs );

} // namespace RakNet
