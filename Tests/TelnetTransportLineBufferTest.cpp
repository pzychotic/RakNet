#include "Plugins/TelnetTransport.h"
#include "RakAssert.h"
#include "RakMemoryOverride.h"
#include "RakNetTypes.h"
#include "SocketDefines.h"
#include "SocketIncludes.h"
#include "TransportInterface.h"
#include "WSAStartupSingleton.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>

/*
Pins that remote input stays inside a TelnetClient's line buffers, and that the up-arrow
recall frees the packet it came in.

Each TelnetClient holds textInput and lastSentTextInput, REMOTE_MAX_TEXT_INPUT bytes each.
Three defects wrote past them from input any connected client can send, before
authentication:

- ReassembleLine accepted a character while the cursor was below REMOTE_MAX_TEXT_INPUT, so
  a full line's terminator landed one past textInput, and Receive copied REMOTE_MAX_TEXT_INPUT
  + 1 bytes into lastSentTextInput.
- ESC [ A walked textInput to its first zero byte - the longest line typed so far, not the
  cursor - overwrote that with backspaces, and appended lastSentTextInput with strcat. A line
  of half the buffer or more, then an up arrow, ran off the end of the heap block.
- That branch returned without deallocating the packet.

It also pins that every line in a segment is delivered. Receive returned the first line a
packet completed and freed the packet, so the bytes after that newline never reached the
line buffer: a second line was lost, and a partial one lost its beginning.

The client is a raw socket, as in TelnetTransportReconnectTest.cpp.
*/

using namespace RakNet;

namespace {

// TCP, past the 31030-31033 that TelnetTransportReconnectTest.cpp takes. CreateListenSocket
// does not set SO_REUSEADDR before it binds, so no port is shared between cases.
constexpr unsigned short kUpArrowListenPort = 31034;
constexpr unsigned short kFullLineListenPort = 31035;
constexpr unsigned short kUpArrowLeakListenPort = 31036;
constexpr unsigned short kTwoLinesListenPort = 31044;
constexpr unsigned short kSplitLineListenPort = 31045;
constexpr unsigned short kManyLinesListenPort = 31046;
constexpr unsigned short kQueuedAtStopListenPort = 31047;

// Loopback, so every wait here is over as soon as the threads have been scheduled once.
// Generous so a loaded machine cannot turn a pass into a failure.
constexpr std::chrono::milliseconds kDeadline( 5000 );

const char kUpArrow[] = { 27, 91, 65 };

// Runs until the predicate holds or the deadline passes; returns whether it held.
template <typename Predicate>
bool WaitFor( Predicate predicate )
{
    const auto deadline = std::chrono::steady_clock::now() + kDeadline;

    while( std::chrono::steady_clock::now() < deadline )
    {
        if( predicate() )
            return true;

        std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
    }

    return predicate();
}

// Nothing here goes through RakPeer::Startup, so the raw sockets take the same Winsock
// refcount RakNet itself does. A no-op off Windows.
struct WinsockScope
{
    WinsockScope() { WSAStartupSingleton::AddRef(); }
    ~WinsockScope() { WSAStartupSingleton::Deref(); }
};

// The cursor is protected; the full-line case checks it came back to the start.
class InspectableTelnetTransport : public TelnetTransport
{
public:
    unsigned CursorPosition() const { return remoteClients.empty() ? 0 : remoteClients.front()->cursorPosition; }
};

// A blocking TCP connection to the listener on loopback.
__TCPSOCKET__ Connect( unsigned short listenPort )
{
    const __TCPSOCKET__ s = socket__( AF_INET, SOCK_STREAM, IPPROTO_TCP );
    REQUIRE( s != (__TCPSOCKET__)-1 );

    sockaddr_in remote;
    memset( &remote, 0, sizeof( remote ) );
    remote.sin_family = AF_INET;
    remote.sin_port = htons( listenPort );
    remote.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
    REQUIRE( connect__( s, (const sockaddr*)&remote, sizeof( remote ) ) == 0 );
    return s;
}

void SendAll( __TCPSOCKET__ s, const char* data, size_t length )
{
    REQUIRE( send__( s, data, (int)length, 0 ) == (int)length );
}

// Starts a transport on port, connects a client and drains its new event.
__TCPSOCKET__ StartAndConnect( TelnetTransport& telnet, unsigned short port )
{
    REQUIRE( telnet.Start( port, true ) );
    const __TCPSOCKET__ client = Connect( port );
    REQUIRE( WaitFor( [&] { return telnet.HasNewIncomingConnection() != UNASSIGNED_SYSTEM_ADDRESS; } ) );
    return client;
}

// Returns the next line Receive gives, or an empty string if none came before the deadline.
std::string ReceiveLine( TelnetTransport& telnet )
{
    Packet* received = 0;
    if( !WaitFor( [&] { return ( received = telnet.Receive() ) != 0; } ) )
        return std::string();

    const std::string line( (const char*)received->data, received->length );
    telnet.DeallocatePacket( received );
    return line;
}

// Sends text and returns the line Receive reassembles from it, or an empty string if none
// came before the deadline.
std::string SendLine( TelnetTransport& telnet, __TCPSOCKET__ client, const std::string& text )
{
    SendAll( client, text.data(), text.size() );
    return ReceiveLine( telnet );
}

// Tracks the rakMalloc_Ex blocks of the armed size that have not been passed to rakFree_Ex.
//
// TCPInterface's receive thread allocates each incoming segment's data as its length plus a
// terminator, and nothing else it or TelnetTransport allocates is that size while armed, so
// armed for a 3-byte ESC [ A this counts the up-arrow packets not yet freed.
//
// The hooks are plain globals, so a case declares its LiveBlocks before its TelnetTransport:
// they go in before Start creates TCPInterface's thread and come out after Stop has waited
// for it. That thread calls them, so the set is locked and the armed size atomic. The state
// is static, so only one LiveBlocks exists at a time.
class LiveBlocks
{
public:
    LiveBlocks()
    {
        RakAssert( s_previousMalloc == 0 );
        s_armedSize = 0;
        {
            std::lock_guard<std::mutex> lock( s_mutex );
            s_live.clear();
        }
        s_previousMalloc = GetMalloc_Ex();
        s_previousFree = GetFree_Ex();
        SetMalloc_Ex( &Malloc );
        SetFree_Ex( &Free );
    }

    ~LiveBlocks()
    {
        SetMalloc_Ex( s_previousMalloc );
        SetFree_Ex( s_previousFree );
        s_previousMalloc = 0;
        s_previousFree = 0;
    }

    LiveBlocks( const LiveBlocks& ) = delete;
    LiveBlocks& operator=( const LiveBlocks& ) = delete;

    // Counts blocks of size allocated from now on, until Disarm.
    void Arm( size_t size ) { s_armedSize = size; }
    void Disarm() { s_armedSize = 0; }

    size_t Count() const
    {
        std::lock_guard<std::mutex> lock( s_mutex );
        return s_live.size();
    }

private:
    static void* Malloc( size_t size, const char* file, unsigned int line )
    {
        void* p = s_previousMalloc( size, file, line );
        const size_t armedSize = s_armedSize;
        if( p != 0 && armedSize != 0 && size == armedSize )
        {
            std::lock_guard<std::mutex> lock( s_mutex );
            s_live.insert( p );
        }
        return p;
    }

    static void Free( void* p, const char* file, unsigned int line )
    {
        {
            std::lock_guard<std::mutex> lock( s_mutex );
            s_live.erase( p );
        }
        s_previousFree( p, file, line );
    }

    static std::atomic<size_t> s_armedSize;
    static void* ( *s_previousMalloc )( size_t, const char*, unsigned int );
    static void ( *s_previousFree )( void*, const char*, unsigned int );
    static std::mutex s_mutex;
    static std::set<void*> s_live;
};

std::atomic<size_t> LiveBlocks::s_armedSize( 0 );
void* ( *LiveBlocks::s_previousMalloc )( size_t, const char*, unsigned int ) = 0;
void ( *LiveBlocks::s_previousFree )( void*, const char*, unsigned int ) = 0;
std::mutex LiveBlocks::s_mutex;
std::set<void*> LiveBlocks::s_live;

// Sends ESC [ A in a segment of its own - Receive only recognises the up arrow as a whole
// 3-byte packet - and has Receive take it. Returns whether the packet was freed after.
bool SendUpArrow( TelnetTransport& telnet, __TCPSOCKET__ client, LiveBlocks& blocks )
{
    blocks.Arm( sizeof( kUpArrow ) + 1 );
    SendAll( client, kUpArrow, sizeof( kUpArrow ) );

    // Received by TCPInterface's thread...
    REQUIRE( WaitFor( [&] { return blocks.Count() == 1; } ) );

    // ...and freed once Receive has taken it. Receive returns nothing for it.
    bool returnedNothing = true;
    const bool freed = WaitFor( [&] {
        returnedNothing = returnedNothing && telnet.Receive() == 0;
        return blocks.Count() == 0;
    } );
    blocks.Disarm();
    CHECK( returnedNothing );
    return freed;
}

} // namespace

TEST_CASE( "TelnetTransport recalls a long line with the up arrow without overrunning it", "[telnettransport][network]" )
{
    WinsockScope winsock;

    LiveBlocks blocks;
    TelnetTransport telnet;
    const __TCPSOCKET__ client = StartAndConnect( telnet, kUpArrowListenPort );

    // Nearly a full buffer, so the old walk-then-strcat built about twice REMOTE_MAX_TEXT_INPUT:
    // past textInput and across the lastSentTextInput it was copying from, which corrupted
    // the recalled line.
    const std::string longLine( REMOTE_MAX_TEXT_INPUT - 48, 'x' );
    REQUIRE( SendLine( telnet, client, longLine + "\n" ) == longLine );

    CHECK( SendUpArrow( telnet, client, blocks ) );

    // The recalled line is the cursor's line now, so Enter sends it again.
    CHECK( SendLine( telnet, client, "\n" ) == longLine );

    closesocket__( client );
    telnet.Stop();
}

TEST_CASE( "TelnetTransport keeps a full line and its terminator inside the line buffer", "[telnettransport][network]" )
{
    WinsockScope winsock;

    InspectableTelnetTransport telnet;
    const __TCPSOCKET__ client = StartAndConnect( telnet, kFullLineListenPort );

    // One printable character more than fits beside a terminator.
    const std::string fullLine( REMOTE_MAX_TEXT_INPUT, 'y' );
    const std::string line = SendLine( telnet, client, fullLine + "\n" );

    // The characters that fit, and no more: the terminator had nowhere else to go.
    CHECK( line.size() == REMOTE_MAX_TEXT_INPUT - 1 );
    CHECK( line == fullLine.substr( 0, REMOTE_MAX_TEXT_INPUT - 1 ) );

    // Unfixed, copying the line wrote into cursorPosition, which follows lastSentTextInput.
    CHECK( telnet.CursorPosition() == 0 );

    // And the next line is not polluted by the one before.
    CHECK( SendLine( telnet, client, "next\n" ) == "next" );

    closesocket__( client );
    telnet.Stop();
}

TEST_CASE( "TelnetTransport frees an up-arrow packet", "[telnettransport][network]" )
{
    WinsockScope winsock;

    LiveBlocks blocks;
    TelnetTransport telnet;
    const __TCPSOCKET__ client = StartAndConnect( telnet, kUpArrowLeakListenPort );

    // One with nothing to recall, and one with a line to recall: both branches.
    for( const bool withLastLine : { false, true } )
    {
        if( withLastLine )
            REQUIRE( SendLine( telnet, client, "recall\n" ) == "recall" );

        CHECK( SendUpArrow( telnet, client, blocks ) );
    }

    closesocket__( client );
    telnet.Stop();
}

TEST_CASE( "TelnetTransport delivers every line in a segment", "[telnettransport][network]" )
{
    WinsockScope winsock;

    TelnetTransport telnet;
    const __TCPSOCKET__ client = StartAndConnect( telnet, kTwoLinesListenPort );

    // One send on loopback, so one segment and one TCPInterface packet.
    const char twoLines[] = "a\nb\n";
    SendAll( client, twoLines, sizeof( twoLines ) - 1 );

    CHECK( ReceiveLine( telnet ) == "a" );
    CHECK( ReceiveLine( telnet ) == "b" );
    CHECK( telnet.Receive() == 0 );

    closesocket__( client );
    telnet.Stop();
}

TEST_CASE( "TelnetTransport keeps the start of a line that follows another in a segment", "[telnettransport][network]" )
{
    WinsockScope winsock;

    TelnetTransport telnet;
    const __TCPSOCKET__ client = StartAndConnect( telnet, kSplitLineListenPort );

    const char lineAndStart[] = "a\nbc";
    SendAll( client, lineAndStart, sizeof( lineAndStart ) - 1 );
    CHECK( ReceiveLine( telnet ) == "a" );

    CHECK( SendLine( telnet, client, "d\n" ) == "bcd" );

    closesocket__( client );
    telnet.Stop();
}

TEST_CASE( "TelnetTransport delivers a segment full of lines in order", "[telnettransport][network]" )
{
    WinsockScope winsock;

    TelnetTransport telnet;
    const __TCPSOCKET__ client = StartAndConnect( telnet, kManyLinesListenPort );

    // As many one-character lines as fit in an Ethernet-sized segment, each a different
    // letter from the one before, so a line out of order shows.
    constexpr size_t kLineCount = 1460 / 2;
    std::string lines;
    for( size_t i = 0; i < kLineCount; i++ )
    {
        lines += (char)( 'a' + i % 26 );
        lines += '\n';
    }
    SendAll( client, lines.data(), lines.size() );

    for( size_t i = 0; i < kLineCount; i++ )
        REQUIRE( ReceiveLine( telnet ) == std::string( 1, (char)( 'a' + i % 26 ) ) );
    CHECK( telnet.Receive() == 0 );

    closesocket__( client );
    telnet.Stop();
}

TEST_CASE( "TelnetTransport frees lines still queued when it stops", "[telnettransport][network]" )
{
    WinsockScope winsock;

    LiveBlocks blocks;
    TelnetTransport telnet;
    const __TCPSOCKET__ client = StartAndConnect( telnet, kQueuedAtStopListenPort );

    // A queued line's data is its length plus a terminator. Nothing else allocated while
    // armed is that size: the TCP packet's data is the whole segment plus a terminator.
    const std::string queued = "queued";
    blocks.Arm( queued.size() + 1 );

    const std::string lines = "a\n" + queued + "\n";
    SendAll( client, lines.data(), lines.size() );
    REQUIRE( ReceiveLine( telnet ) == "a" );

    // The second line waits for a Receive that never comes.
    CHECK( blocks.Count() == 1 );

    closesocket__( client );
    telnet.Stop();
    CHECK( blocks.Count() == 0 );
}
