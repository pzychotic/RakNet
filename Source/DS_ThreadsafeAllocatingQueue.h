/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

/// \file DS_ThreadsafeAllocatingQueue.h
/// \internal
/// A threadsafe queue, that also uses a memory pool for allocation

#pragma once

#include "DS_Queue.h"
#include "DS_MemoryPool.h"

#include <mutex>

// #if defined(new)
// #pragma push_macro("new")
// #undef new
// #define RMO_NEW_UNDEF_ALLOCATING_QUEUE
// #endif

namespace RakNet { namespace DataStructures {

template<class structureType>
class RAK_DLL_EXPORT ThreadsafeAllocatingQueue
{
public:
    // Queue operations
    void Push( structureType* s );
    structureType* PopInaccurate( void );
    structureType* Pop( void );
    void SetPageSize( int size );
    bool IsEmpty( void );
    structureType* operator[]( unsigned int position );
    void RemoveAtIndex( unsigned int position );
    unsigned int Size( void );

    // Memory pool operations
    structureType* Allocate( const char* file, unsigned int line );
    void Deallocate( structureType* s, const char* file, unsigned int line );
    void Clear( const char* file, unsigned int line );

protected:
    mutable MemoryPool<structureType> memoryPool;
    std::mutex memoryPoolMutex;
    Queue<structureType*> queue;
    std::mutex queueMutex;
};

template<class structureType>
void ThreadsafeAllocatingQueue<structureType>::Push( structureType* s )
{
    std::lock_guard<std::mutex> guard( queueMutex );
    queue.Push( s, _FILE_AND_LINE_ );
}

template<class structureType>
structureType* ThreadsafeAllocatingQueue<structureType>::PopInaccurate( void )
{
    structureType* s;
    if( queue.IsEmpty() )
        return 0;
    std::lock_guard<std::mutex> guard( queueMutex );
    if( queue.IsEmpty() == false )
        s = queue.Pop();
    else
        s = 0;
    return s;
}

template<class structureType>
structureType* ThreadsafeAllocatingQueue<structureType>::Pop( void )
{
    std::lock_guard<std::mutex> guard( queueMutex );
    if( queue.IsEmpty() )
    {
        return 0;
    }
    structureType* s = queue.Pop();
    return s;
}

template<class structureType>
structureType* ThreadsafeAllocatingQueue<structureType>::Allocate( const char* file, unsigned int line )
{
    structureType* s;
    memoryPoolMutex.lock();
    s = memoryPool.Allocate( file, line );
    memoryPoolMutex.unlock();
    // Call new operator, memoryPool doesn't do this
    s = new( (void*)s ) structureType;
    return s;
}
template<class structureType>
void ThreadsafeAllocatingQueue<structureType>::Deallocate( structureType* s, const char* file, unsigned int line )
{
    // Call delete operator, memory pool doesn't do this
    s->~structureType();
    memoryPoolMutex.lock();
    memoryPool.Release( s, file, line );
    memoryPoolMutex.unlock();
}

template<class structureType>
void ThreadsafeAllocatingQueue<structureType>::Clear( const char* file, unsigned int line )
{
    memoryPoolMutex.lock();
    for( unsigned int i = 0; i < queue.Size(); i++ )
    {
        queue[i]->~structureType();
        memoryPool.Release( queue[i], file, line );
    }
    queue.Clear( file, line );
    memoryPoolMutex.unlock();
    memoryPoolMutex.lock();
    memoryPool.Clear( file, line );
    memoryPoolMutex.unlock();
}

template<class structureType>
void ThreadsafeAllocatingQueue<structureType>::SetPageSize( int size )
{
    memoryPool.SetPageSize( size );
}

template<class structureType>
bool ThreadsafeAllocatingQueue<structureType>::IsEmpty( void )
{
    std::lock_guard<std::mutex> queueGuard( queueMutex );
    bool isEmpty = queue.IsEmpty();
    return isEmpty;
}

template<class structureType>
structureType* ThreadsafeAllocatingQueue<structureType>::operator[]( unsigned int position )
{
    std::lock_guard<std::mutex> queueGuard( queueMutex );
    structureType* s = queue[position];
    return s;
}

template<class structureType>
void ThreadsafeAllocatingQueue<structureType>::RemoveAtIndex( unsigned int position )
{
    std::lock_guard<std::mutex> queueGuard( queueMutex );
    queue.RemoveAtIndex( position );
}

template<class structureType>
unsigned int ThreadsafeAllocatingQueue<structureType>::Size( void )
{
    std::lock_guard<std::mutex> queueGuard( queueMutex );
    unsigned int s = queue.Size();
    return s;
}

}} // namespace RakNet::DataStructures

// #if defined(RMO_NEW_UNDEF_ALLOCATING_QUEUE)
// #pragma pop_macro("new")
// #undef RMO_NEW_UNDEF_ALLOCATING_QUEUE
// #endif
