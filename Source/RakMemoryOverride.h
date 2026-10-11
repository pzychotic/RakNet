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
/// \brief The rakMalloc family of allocation hooks, and the OP_NEW templates.
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
///   when rakMalloc or one of its friends returns null, at the sites that call them
///   directly and report the failure by return value. Every other allocation, std
///   containers and std::string included, fails through std::bad_alloc and never calls it.
/// - **SetMalloc and friends redirect only the rakMalloc family.** To control all of
///   RakNet's memory, containers included, replace the global operator new and operator
///   delete.
/// - **The OP_NEW family is plain new and delete,** kept for embedders' code. Source/ does
///   not use it.
///

#pragma once

#include "Export.h"
#include "RakNetDefines.h"
#include <cstdlib>
#include <new>

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

template<class Type>
RAK_DLL_EXPORT Type* OP_NEW( const char* file, unsigned int line )
{
    (void)file;
    (void)line;
    return new Type;
}

template<class Type, class P1>
RAK_DLL_EXPORT Type* OP_NEW_1( const char* file, unsigned int line, const P1& p1 )
{
    (void)file;
    (void)line;
    return new Type( p1 );
}

template<class Type, class P1, class P2>
RAK_DLL_EXPORT Type* OP_NEW_2( const char* file, unsigned int line, const P1& p1, const P2& p2 )
{
    (void)file;
    (void)line;
    return new Type( p1, p2 );
}

template<class Type, class P1, class P2, class P3>
RAK_DLL_EXPORT Type* OP_NEW_3( const char* file, unsigned int line, const P1& p1, const P2& p2, const P3& p3 )
{
    (void)file;
    (void)line;
    return new Type( p1, p2, p3 );
}

template<class Type, class P1, class P2, class P3, class P4>
RAK_DLL_EXPORT Type* OP_NEW_4( const char* file, unsigned int line, const P1& p1, const P2& p2, const P3& p3, const P4& p4 )
{
    (void)file;
    (void)line;
    return new Type( p1, p2, p3, p4 );
}


// count is unsigned so an unsigned caller's value cannot arrive negative: new Type[negative]
// throws std::bad_array_new_length, which is bad input rather than allocation failure.
template<class Type>
RAK_DLL_EXPORT Type* OP_NEW_ARRAY( const size_t count, const char* file, unsigned int line )
{
    if( count == 0 )
        return 0;

    (void)file;
    (void)line;
    return new Type[count];
}

template<class Type>
RAK_DLL_EXPORT void OP_DELETE( Type* buff, const char* file, unsigned int line )
{
    (void)file;
    (void)line;
    delete buff;
}

template<class Type>
RAK_DLL_EXPORT void OP_DELETE_ARRAY( Type* buff, const char* file, unsigned int line )
{
    (void)file;
    (void)line;
    delete[] buff;
}

void RAK_DLL_EXPORT* _RakMalloc( size_t size );
void RAK_DLL_EXPORT* _RakRealloc( void* p, size_t size );
void RAK_DLL_EXPORT  _RakFree( void* p );
void RAK_DLL_EXPORT* _RakMalloc_Ex( size_t size, const char* file, unsigned int line );
void RAK_DLL_EXPORT* _RakRealloc_Ex( void* p, size_t size, const char* file, unsigned int line );
void RAK_DLL_EXPORT  _RakFree_Ex( void* p, const char* file, unsigned int line );

} // namespace RakNet
