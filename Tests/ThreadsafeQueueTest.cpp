#include "ThreadsafeQueue.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <thread>

/*
ThreadsafeQueue carries items by value from one thread to another: RakPeer's buffered
commands, TCPInterface's received messages and connection events, UDPForwarder's
requests. These tests use it from one thread, except the last, which pushes from two.
*/

using namespace RakNet;

TEST_CASE( "ThreadsafeQueue pops items in the order they were pushed", "[threadsafequeue]" )
{
    ThreadsafeQueue<std::unique_ptr<int>> queue;
    queue.Push( std::make_unique<int>( 1 ) );
    queue.Push( std::make_unique<int>( 2 ) );

    std::optional<std::unique_ptr<int>> first = queue.Pop();
    REQUIRE( first.has_value() );
    CHECK( **first == 1 );
    std::optional<std::unique_ptr<int>> second = queue.Pop();
    REQUIRE( second.has_value() );
    CHECK( **second == 2 );
}

TEST_CASE( "ThreadsafeQueue pops nothing once it is empty", "[threadsafequeue]" )
{
    ThreadsafeQueue<int> queue;
    CHECK_FALSE( queue.Pop().has_value() );

    queue.Push( 7 );
    CHECK( queue.Pop() == std::optional<int>( 7 ) );
    CHECK_FALSE( queue.Pop().has_value() );
}

TEST_CASE( "ThreadsafeQueue counts what it holds, and Clear drops it all", "[threadsafequeue]" )
{
    ThreadsafeQueue<int> queue;
    CHECK( queue.IsEmpty() );
    CHECK( queue.Size() == 0 );

    queue.Push( 1 );
    queue.Push( 2 );
    queue.Push( 3 );
    CHECK_FALSE( queue.IsEmpty() );
    CHECK( queue.Size() == 3 );

    (void)queue.Pop();
    CHECK( queue.Size() == 2 );

    queue.Clear();
    CHECK( queue.IsEmpty() );
    CHECK_FALSE( queue.Pop().has_value() );
}

TEST_CASE( "ThreadsafeQueue destroys what it held when cleared", "[threadsafequeue]" )
{
    std::shared_ptr<int> tracked = std::make_shared<int>( 0 );
    ThreadsafeQueue<std::shared_ptr<int>> queue;
    queue.Push( tracked );
    CHECK( tracked.use_count() == 2 );

    queue.Clear();
    CHECK( tracked.use_count() == 1 );
}

TEST_CASE( "ThreadsafeQueue keeps every item two threads push at once", "[threadsafequeue]" )
{
    constexpr int perThread = 10000;
    ThreadsafeQueue<int> queue;
    auto pushRange = [&]( int first ) {
        for( int i = first; i < first + perThread; i++ )
            queue.Push( i );
    };
    std::thread a( pushRange, 0 );
    std::thread b( pushRange, perThread );
    a.join();
    b.join();

    REQUIRE( queue.Size() == 2 * perThread );
    // Each thread's items come out in the order it pushed them.
    int lastA = -1, lastB = perThread - 1;
    while( std::optional<int> item = queue.Pop() )
    {
        int& last = *item < perThread ? lastA : lastB;
        CHECK( *item == last + 1 );
        last = *item;
    }
    CHECK( lastA == perThread - 1 );
    CHECK( lastB == 2 * perThread - 1 );
}
