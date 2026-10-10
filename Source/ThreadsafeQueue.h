#pragma once

#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace RakNet {

/// A queue of items held by value, safe to push and pop from different threads.
///
/// Growth that cannot be allocated is fatal (ADR-0004).
template<class T>
class ThreadsafeQueue
{
public:
    void Push( T item )
    {
        std::lock_guard<std::mutex> guard( mutex );
        items.push_back( std::move( item ) );
    }

    /// The oldest item, or nothing if the queue is empty.
    std::optional<T> Pop()
    {
        std::lock_guard<std::mutex> guard( mutex );
        if( items.empty() )
            return std::nullopt;
        std::optional<T> item( std::move( items.front() ) );
        items.pop_front();
        return item;
    }

    bool IsEmpty() const
    {
        std::lock_guard<std::mutex> guard( mutex );
        return items.empty();
    }

    size_t Size() const
    {
        std::lock_guard<std::mutex> guard( mutex );
        return items.size();
    }

    /// Destroys every item, outside the lock.
    void Clear()
    {
        std::deque<T> dropped;
        {
            std::lock_guard<std::mutex> guard( mutex );
            dropped.swap( items );
        }
    }

private:
    mutable std::mutex mutex;
    std::deque<T> items;
};

} // namespace RakNet
