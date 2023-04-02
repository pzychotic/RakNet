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

#include "DS_MemoryPool.h"

#include <deque>
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
    unsigned int Size( void );

    // Memory pool operations
    structureType* Allocate( const char* file, unsigned int line );
    void Deallocate( structureType* s, const char* file, unsigned int line );
    void Clear( const char* file, unsigned int line );

protected:
    mutable MemoryPool<structureType> memoryPool;
    std::mutex memoryPoolMutex;
    std::deque<structureType*> queue;
    std::mutex queueMutex;
};

template<class structureType>
void ThreadsafeAllocatingQueue<structureType>::Push( structureType* s )
{
    std::lock_guard<std::mutex> guard( queueMutex );
    queue.push_back( s );
}

template<class structureType>
structureType* ThreadsafeAllocatingQueue<structureType>::PopInaccurate( void )
{
    structureType* s;
    if( queue.empty() )
        return 0;
    std::lock_guard<std::mutex> guard( queueMutex );
    if( !queue.empty() )
    {
        s = queue.front();
        queue.pop_front();
    }
    else
        s = 0;
    return s;
}

template<class structureType>
structureType* ThreadsafeAllocatingQueue<structureType>::Pop( void )
{
    std::lock_guard<std::mutex> guard( queueMutex );
    if( queue.empty() )
    {
        return 0;
    }
    structureType* s = queue.front();
    queue.pop_front();
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
    for( structureType* s : queue )
    {
        s->~structureType();
        memoryPool.Release( s, file, line );
    }
    queue.clear();
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
    return queue.empty();
}

template<class structureType>
unsigned int ThreadsafeAllocatingQueue<structureType>::Size( void )
{
    std::lock_guard<std::mutex> queueGuard( queueMutex );
    return queue.size();
}

}} // namespace RakNet::DataStructures

// #if defined(RMO_NEW_UNDEF_ALLOCATING_QUEUE)
// #pragma pop_macro("new")
// #undef RMO_NEW_UNDEF_ALLOCATING_QUEUE
// #endif
