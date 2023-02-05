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
    RakString( const char* format, ... );
    RakString( const RakString& rhs );
    ~RakString();

    /// Same as std::string::c_str
    const char* C_String( void ) const { return sharedString->c_str; }

    /// Assigment operators
    RakString& operator=( const RakString& rhs );
    RakString& operator=( const char* str );
    RakString& operator=( const char c );

    /// Concatenation
    RakString& operator+=( const RakString& rhs );
    RakString& operator+=( const char* str );
    RakString& operator+=( const char c );

    /// Equality
    bool operator==( const RakString& rhs ) const;
    bool operator==( const char* str ) const;

    // Comparison
    bool operator<( const RakString& right ) const;
    bool operator<=( const RakString& right ) const;
    bool operator>( const RakString& right ) const;
    bool operator>=( const RakString& right ) const;

    /// Inequality
    bool operator!=( const RakString& rhs ) const;
    bool operator!=( const char* str ) const;

    /// Set the value of the string
    void Set( const char* format, ... );

    /// Returns if the string is empty. Also, C_String() would return ""
    bool IsEmpty( void ) const;

    /// Returns the length of the string
    size_t GetLength( void ) const;

    /// Create a RakString with a value, without doing printf style parsing
    /// Equivalent to assignment operator
    static RakString NonVariadic( const char* str );

    /// Hash the string into an unsigned int
    static unsigned long ToInteger( const RakString& rs );

    /// Clear the string
    void Clear( void );

    /// RakString uses a freeList of old no-longer used strings
    /// Call this function to clear this memory on shutdown
    static void FreeMemory( void );
    /// \internal
    static void FreeMemoryNoMutex( void );

    /// \internal
    static size_t GetSizeToAllocate( size_t bytes )
    {
        const size_t smallStringSize = 128 - sizeof( unsigned int ) - sizeof( size_t ) - sizeof( char* ) * 2;
        return bytes <= smallStringSize ? smallStringSize : bytes * 2;
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

    void Allocate( size_t len );
    void Assign( const char* str );
    void Assign( const char* str, va_list ap );

    void Clone( void );
    void Free( void );
    void Realloc( SharedString* sharedString, size_t bytes );
};

const RakString RAK_DLL_EXPORT operator+( const RakString& lhs, const RakString& rhs );

} // namespace RakNet
