/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

/// \file
/// \brief The rakMalloc family of allocation hooks, and the OP_NEW templates that use it
/// when _USE_RAK_MEMORY_OVERRIDE is 1.
///
/// These hooks reach less than their names suggest. Out-of-memory and allocator control
/// for the whole library go through the standard hooks instead (ADR-0004):
///
/// - **The library-wide OOM hook is std::set_new_handler.** Allocation failure through
///   operator new, std containers and std::string is fatal. The runtime calls the new
///   handler before it throws std::bad_alloc, whether or not RakNet was built with
///   exceptions, so that is where to log, release a reserve, or abort deliberately. Do not rely on
///   std::set_terminate: on MSVC the failure is a fail-fast, which skips it. Catching
///   std::bad_alloc, or any exception, thrown through RakNet is unsupported.
/// - **SetNotifyOutOfMemory covers only the null-returning rakMalloc paths.** It fires
///   when rakMalloc or one of its friends returns null: at the sites that call them
///   directly and report the failure by return value, and, under _USE_RAK_MEMORY_OVERRIDE,
///   in the OP_NEW templates before the new handler runs. Every other allocation, std
///   containers and std::string included, fails through std::bad_alloc and never calls it.
/// - **SetMalloc and friends redirect only the rakMalloc family.** To control all of
///   RakNet's memory, containers included, replace the global operator new and operator
///   delete.
/// - **_USE_RAK_MEMORY_OVERRIDE is frozen.** It is kept and correct, but it only routes the
///   OP_NEW templates through rakMalloc_Ex and cannot reach containers, so it is not the
///   recommended hook.
///

#pragma once

#include "Export.h"
#include "RakNetDefines.h"
#include <cstdlib>
#include <new>

// #if _USE_RAK_MEMORY_OVERRIDE==1
//  #if defined(new)
//      #pragma push_macro("new")
//      #undef new
//      #define RMO_NEW_UNDEF
//  #endif
// #endif


namespace RakNet {

// These pointers are statically and globally defined in RakMemoryOverride.cpp
// Change them to point to your own allocators if you want.
// Use the functions for a DLL, or just reassign the variable if using source
extern RAK_DLL_EXPORT void* ( *rakMalloc )( size_t size );
extern RAK_DLL_EXPORT void* ( *rakRealloc )( void* p, size_t size );
extern RAK_DLL_EXPORT void  ( *rakFree )( void* p );
extern RAK_DLL_EXPORT void* ( *rakMalloc_Ex )( size_t size, const char* file, unsigned int line );
extern RAK_DLL_EXPORT void* ( *rakRealloc_Ex )( void* p, size_t size, const char* file, unsigned int line );
extern RAK_DLL_EXPORT void  ( *rakFree_Ex )( void* p, const char* file, unsigned int line );
extern RAK_DLL_EXPORT void  ( *notifyOutOfMemory )( const char* file, const long line );

// Change to a user defined allocation function. These redirect only the rakMalloc family,
// not operator new or the std containers; see the file comment. The pointers are plain
// globals, read by every RakNet thread unsynchronized: set them before any RakNet thread
// starts, and restore them only after the last one has stopped.
void RAK_DLL_EXPORT SetMalloc( void* ( *userFunction )( size_t size ) );
void RAK_DLL_EXPORT SetRealloc( void* ( *userFunction )( void* p, size_t size ) );
void RAK_DLL_EXPORT SetFree( void ( *userFunction )( void* p ) );
void RAK_DLL_EXPORT SetMalloc_Ex( void* ( *userFunction )( size_t size, const char* file, unsigned int line ) );
void RAK_DLL_EXPORT SetRealloc_Ex( void* ( *userFunction )( void* p, size_t size, const char* file, unsigned int line ) );
void RAK_DLL_EXPORT SetFree_Ex( void ( *userFunction )( void* p, const char* file, unsigned int line ) );
// Change to a user defined out of memory function. It is called only when a rakMalloc-family
// allocation returns null, not for allocation failure in general: the library-wide OOM hook
// is std::set_new_handler. See the file comment.
void RAK_DLL_EXPORT SetNotifyOutOfMemory( void ( *userFunction )( const char* file, const long line ) );

extern RAK_DLL_EXPORT void* ( *GetMalloc() )( size_t size );
extern RAK_DLL_EXPORT void* ( *GetRealloc() )( void* p, size_t size );
extern RAK_DLL_EXPORT void  ( *GetFree() )( void* p );
extern RAK_DLL_EXPORT void* ( *GetMalloc_Ex() )( size_t size, const char* file, unsigned int line );
extern RAK_DLL_EXPORT void* ( *GetRealloc_Ex() )( void* p, size_t size, const char* file, unsigned int line );
extern RAK_DLL_EXPORT void  ( *GetFree_Ex() )( void* p, const char* file, unsigned int line );

// Allocates through rakMalloc_Ex for the _USE_RAK_MEMORY_OVERRIDE==1 templates below, and
// never returns null. Other rakMalloc sites notify and return failure (ADR-0004), but the
// OP_NEW family has no failure value and its callers assume success, so this fails the way
// operator new does in the ==0 branch. Exported because the templates instantiate in the
// embedder's code.
void RAK_DLL_EXPORT* RakAllocateOrAbort( size_t size, const char* file, unsigned int line );

template<class Type>
RAK_DLL_EXPORT Type* OP_NEW( const char* file, unsigned int line )
{
#if _USE_RAK_MEMORY_OVERRIDE == 1
    char* buffer = (char*)RakAllocateOrAbort( sizeof( Type ), file, line );
    Type* t = new( buffer ) Type;
    return t;
#else
    (void)file;
    (void)line;
    return new Type;
#endif
}

template<class Type, class P1>
RAK_DLL_EXPORT Type* OP_NEW_1( const char* file, unsigned int line, const P1& p1 )
{
#if _USE_RAK_MEMORY_OVERRIDE == 1
    char* buffer = (char*)RakAllocateOrAbort( sizeof( Type ), file, line );
    Type* t = new( buffer ) Type( p1 );
    return t;
#else
    (void)file;
    (void)line;
    return new Type( p1 );
#endif
}

template<class Type, class P1, class P2>
RAK_DLL_EXPORT Type* OP_NEW_2( const char* file, unsigned int line, const P1& p1, const P2& p2 )
{
#if _USE_RAK_MEMORY_OVERRIDE == 1
    char* buffer = (char*)RakAllocateOrAbort( sizeof( Type ), file, line );
    Type* t = new( buffer ) Type( p1, p2 );
    return t;
#else
    (void)file;
    (void)line;
    return new Type( p1, p2 );
#endif
}

template<class Type, class P1, class P2, class P3>
RAK_DLL_EXPORT Type* OP_NEW_3( const char* file, unsigned int line, const P1& p1, const P2& p2, const P3& p3 )
{
#if _USE_RAK_MEMORY_OVERRIDE == 1
    char* buffer = (char*)RakAllocateOrAbort( sizeof( Type ), file, line );
    Type* t = new( buffer ) Type( p1, p2, p3 );
    return t;
#else
    (void)file;
    (void)line;
    return new Type( p1, p2, p3 );
#endif
}

template<class Type, class P1, class P2, class P3, class P4>
RAK_DLL_EXPORT Type* OP_NEW_4( const char* file, unsigned int line, const P1& p1, const P2& p2, const P3& p3, const P4& p4 )
{
#if _USE_RAK_MEMORY_OVERRIDE == 1
    char* buffer = (char*)RakAllocateOrAbort( sizeof( Type ), file, line );
    Type* t = new( buffer ) Type( p1, p2, p3, p4 );
    return t;
#else
    (void)file;
    (void)line;
    return new Type( p1, p2, p3, p4 );
#endif
}


// count is unsigned so an unsigned caller's value cannot arrive negative: new Type[negative]
// throws std::bad_array_new_length, which is bad input rather than allocation failure.
template<class Type>
RAK_DLL_EXPORT Type* OP_NEW_ARRAY( const size_t count, const char* file, unsigned int line )
{
    if( count == 0 )
        return 0;

#if _USE_RAK_MEMORY_OVERRIDE == 1
    // A byte size that does not fit in size_t is bad input, which callers bound (ADR-0004),
    // not memory exhaustion, so it skips notifyOutOfMemory and the new handler. This is
    // only the backstop: new[] ends the ==0 branch with std::bad_array_new_length, and
    // letting the size wrap would construct past the end of a short buffer.
    if( count > ( ( size_t )-1 - sizeof( size_t ) ) / sizeof( Type ) )
        std::abort();
    char* buffer = (char*)RakAllocateOrAbort( sizeof( size_t ) + sizeof( Type ) * count, file, line );
    ( (size_t*)buffer )[0] = count;
    for( size_t i = 0; i < count; i++ )
    {
        new( buffer + sizeof( size_t ) + i * sizeof( Type ) ) Type;
    }
    return (Type*)( buffer + sizeof( size_t ) );
#else
    (void)file;
    (void)line;
    return new Type[count];
#endif
}

template<class Type>
RAK_DLL_EXPORT void OP_DELETE( Type* buff, const char* file, unsigned int line )
{
#if _USE_RAK_MEMORY_OVERRIDE == 1
    if( buff == 0 )
        return;
    buff->~Type();
    ( GetFree_Ex() )( (char*)buff, file, line );
#else
    (void)file;
    (void)line;
    delete buff;
#endif
}

template<class Type>
RAK_DLL_EXPORT void OP_DELETE_ARRAY( Type* buff, const char* file, unsigned int line )
{
#if _USE_RAK_MEMORY_OVERRIDE == 1
    if( buff == 0 )
        return;

    size_t count = ( (size_t*)( (char*)buff - sizeof( size_t ) ) )[0];
    Type* t;
    for( size_t i = 0; i < count; i++ )
    {
        t = buff + i;
        t->~Type();
    }
    ( GetFree_Ex() )( (char*)buff - sizeof( size_t ), file, line );
#else
    (void)file;
    (void)line;
    delete[] buff;
#endif
}

void RAK_DLL_EXPORT* _RakMalloc( size_t size );
void RAK_DLL_EXPORT* _RakRealloc( void* p, size_t size );
void RAK_DLL_EXPORT  _RakFree( void* p );
void RAK_DLL_EXPORT* _RakMalloc_Ex( size_t size, const char* file, unsigned int line );
void RAK_DLL_EXPORT* _RakRealloc_Ex( void* p, size_t size, const char* file, unsigned int line );
void RAK_DLL_EXPORT  _RakFree_Ex( void* p, const char* file, unsigned int line );

} // namespace RakNet

// #if _USE_RAK_MEMORY_OVERRIDE==1
//  #if defined(RMO_NEW_UNDEF)
//  #pragma pop_macro("new")
//  #undef RMO_NEW_UNDEF
//  #endif
// #endif
