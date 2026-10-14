// [DEBUG-stall] Diagnostic probes for the CI-only network-thread stall. Not for master.
//
// A test sets watchedPeer before Startup. The watched peer's network and receive threads
// then record the last checkpoint they reached and when, so a test that times out can say
// where the thread stood. Compiled only with RAKNET_DIAG_STALL.

#pragma once

#if defined( RAKNET_DIAG_STALL )

#include "GetTime.h"

#include <atomic>
#include <cstdint>

namespace RakNet::DiagStall {

enum Checkpoint : int
{
    DS_NONE = 0,
    DS_THREAD_CREATE_CALLED, // Startup, just before RakThread::Create of the update thread
    DS_THREAD_CREATE_RETURNED,
    DS_LOOP_ENTERED, // first instruction of UpdateNetworkLoop
    DS_BEFORE_USER_MUTEX,
    DS_BEFORE_CYCLE,
    DS_BEFORE_POP, // HandleBufferedPackets, before PopBufferedPacket's lock
    DS_AFTER_POP,
    DS_BEFORE_PROCESS,
    DS_AFTER_PROCESS,
    DS_BEFORE_COMMANDS,
    DS_BEFORE_PUBLISH,
    DS_AFTER_CYCLE,
    DS_BEFORE_WAIT,
    DS_AFTER_WAIT,
    DS_COUNT
};

inline const char* Name( int cp )
{
    static const char* names[DS_COUNT] = { "NONE", "THREAD_CREATE_CALLED", "THREAD_CREATE_RETURNED", "LOOP_ENTERED", "BEFORE_USER_MUTEX", "BEFORE_CYCLE", "BEFORE_POP", "AFTER_POP", "BEFORE_PROCESS", "AFTER_PROCESS", "BEFORE_COMMANDS", "BEFORE_PUBLISH", "AFTER_CYCLE", "BEFORE_WAIT", "AFTER_WAIT" };
    return cp >= 0 && cp < DS_COUNT ? names[cp] : "?";
}

inline std::atomic<const void*> watchedPeer{ nullptr };

// Update thread.
inline std::atomic<int> lastCheckpoint{ DS_NONE };
inline std::atomic<uint64_t> lastCheckpointUs{ 0 };
inline std::atomic<uint64_t> firstUs[DS_COUNT];
inline std::atomic<uint64_t> hits[DS_COUNT];
inline std::atomic<unsigned long> updateThreadId{ 0 };
inline std::atomic<uint64_t> cycles{ 0 };
inline std::atomic<uint64_t> datagramsProcessed{ 0 };
inline std::atomic<uint64_t> maxWaitUs{ 0 };  // longest WaitOnEvent
inline std::atomic<uint64_t> maxCycleUs{ 0 }; // longest RunUpdateCycle

// Receive thread of the watched peer.
inline std::atomic<uint64_t> recvPushed{ 0 };
inline std::atomic<uint64_t> recvFirstUs{ 0 };
inline std::atomic<uint64_t> recvLastUs{ 0 };

inline void Reset( const void* peer )
{
    lastCheckpoint = DS_NONE;
    lastCheckpointUs = 0;
    for( int i = 0; i < DS_COUNT; ++i )
    {
        firstUs[i] = 0;
        hits[i] = 0;
    }
    updateThreadId = 0;
    cycles = 0;
    datagramsProcessed = 0;
    maxWaitUs = 0;
    maxCycleUs = 0;
    recvPushed = 0;
    recvFirstUs = 0;
    recvLastUs = 0;
    watchedPeer = peer;
}

inline void Mark( const void* peer, int cp )
{
    if( peer == nullptr || peer != watchedPeer.load( std::memory_order_relaxed ) )
        return;
    const uint64_t now = RakNet::GetTimeUS();
    uint64_t expected = 0;
    firstUs[cp].compare_exchange_strong( expected, now );
    hits[cp].fetch_add( 1, std::memory_order_relaxed );
    lastCheckpointUs.store( now, std::memory_order_relaxed );
    lastCheckpoint.store( cp, std::memory_order_release );
}

inline void Max( std::atomic<uint64_t>& into, uint64_t value )
{
    uint64_t seen = into.load( std::memory_order_relaxed );
    while( value > seen && !into.compare_exchange_weak( seen, value, std::memory_order_relaxed ) )
    {
    }
}

inline void RecvPushed( const void* peer )
{
    if( peer == nullptr || peer != watchedPeer.load( std::memory_order_relaxed ) )
        return;
    const uint64_t now = RakNet::GetTimeUS();
    uint64_t expected = 0;
    recvFirstUs.compare_exchange_strong( expected, now );
    recvLastUs = now;
    recvPushed.fetch_add( 1, std::memory_order_relaxed );
}

// Timed sections inside the plugin's logging path, recorded only while a peer is watched.
enum Section : int
{
    SEC_NONE = 0,
    SEC_FORMAT_LINE,
    SEC_GET_LOCAL_TIME,
    SEC_FILE_LOCK,
    SEC_FPRINTF,
    SEC_FFLUSH,
    SEC_TO_STRING,
    SEC_IS_BANNED,
    SEC_COUNT
};

inline const char* SectionName( int s )
{
    static const char* names[SEC_COUNT] = { "NONE", "FORMAT_LINE", "GET_LOCAL_TIME", "FILE_LOCK", "FPRINTF", "FFLUSH", "TO_STRING", "IS_BANNED" };
    return s >= 0 && s < SEC_COUNT ? names[s] : "?";
}

// Set on the watched peer's network thread only; sections time that thread alone.
inline thread_local bool onWatchedThread = false;

inline std::atomic<int> currentSection{ SEC_NONE };
inline std::atomic<uint64_t> currentSectionStartUs{ 0 };
inline std::atomic<uint64_t> sectionMaxUs[SEC_COUNT];
inline std::atomic<uint64_t> sectionCount[SEC_COUNT];

inline void ResetSections()
{
    currentSection = SEC_NONE;
    currentSectionStartUs = 0;
    for( int i = 0; i < SEC_COUNT; ++i )
    {
        sectionMaxUs[i] = 0;
        sectionCount[i] = 0;
    }
}

// Times a section; nesting records the innermost one as current.
struct SectionTimer
{
    int section;
    int outer;
    uint64_t outerStartUs;
    uint64_t startUs;
    bool active;

    explicit SectionTimer( int s )
    : section( s )
    , outer( SEC_NONE )
    , outerStartUs( 0 )
    , startUs( 0 )
    , active( onWatchedThread )
    {
        if( !active )
            return;
        startUs = RakNet::GetTimeUS();
        outer = currentSection.load();
        outerStartUs = currentSectionStartUs.load();
        currentSectionStartUs = startUs;
        currentSection = s;
    }
    ~SectionTimer()
    {
        if( !active )
            return;
        Max( sectionMaxUs[section], RakNet::GetTimeUS() - startUs );
        sectionCount[section].fetch_add( 1, std::memory_order_relaxed );
        currentSection = outer;
        currentSectionStartUs = outerStartUs;
    }
};

} // namespace RakNet::DiagStall

#define RAKNET_DIAG_MARK( peer, cp ) ::RakNet::DiagStall::Mark( peer, ::RakNet::DiagStall::cp )
#define RAKNET_DIAG_SECTION( name, s ) ::RakNet::DiagStall::SectionTimer name( ::RakNet::DiagStall::s )
#else
#define RAKNET_DIAG_MARK( peer, cp ) ( (void)0 )
#define RAKNET_DIAG_SECTION( name, s ) ( (void)0 )
#endif
