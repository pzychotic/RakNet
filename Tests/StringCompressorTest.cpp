#include "BitStream.h"
#include "RakMemoryOverride.h"
#include "StringCompressor.h"
#include "StringCompressorScope.h"

#include <catch2/catch_test_macros.hpp>

#include <climits>
#include <cstddef>
#include <string>
#include <vector>

/*
StringCompressor::DecodeString( std::string&, ... ) sizes its buffer from the stream's
stringBitLength, not from the caller's maxCharsToWrite. Every Huffman code is at least one
bit, so a string of stringBitLength bits decodes to at most stringBitLength characters,
and the buffer is min( maxCharsToWrite, stringBitLength + 1 ) bytes. Truncation matches
the char* overload: at most maxCharsToWrite - 1 characters come back.
*/

namespace
{

std::string Decode( const std::string& sent, int maxCharsToWrite )
{
    RakNet::BitStream bitStream;
    RakNet::StringCompressor::Instance()->EncodeString( sent, 0, &bitStream );

    std::string received = "not overwritten";
    REQUIRE( RakNet::StringCompressor::Instance()->DecodeString( received, maxCharsToWrite, &bitStream ) );
    CHECK( bitStream.GetNumberOfUnreadBits() == 0 );
    return received;
}

/// Records the largest rakMalloc_Ex while in scope. Global and not thread-safe, so it is
/// only used here, with no Peer running.
class LargestMalloc
{
public:
    LargestMalloc()
    {
        s_largest = 0;
        s_previous = RakNet::GetMalloc_Ex();
        RakNet::SetMalloc_Ex( &Intercept );
    }
    ~LargestMalloc() { RakNet::SetMalloc_Ex( s_previous ); }

    LargestMalloc( const LargestMalloc& ) = delete;
    LargestMalloc& operator=( const LargestMalloc& ) = delete;

    static size_t Largest() { return s_largest; }

private:
    static void* Intercept( size_t size, const char* file, unsigned int line )
    {
        if( size > s_largest )
            s_largest = size;
        return s_previous( size, file, line );
    }

    static inline size_t s_largest = 0;
    static inline void* ( *s_previous )( size_t, const char*, unsigned int ) = nullptr;
};

} // namespace

TEST_CASE( "A std::string decode longer than maxCharsToWrite keeps maxCharsToWrite - 1 characters", "[stringcompressor]" )
{
    StringCompressorScope compressor;

    CHECK( Decode( "The quick brown fox jumps over the lazy dog", 8 ) == "The qui" );
}

TEST_CASE( "A std::string decode of exactly maxCharsToWrite - 1 characters comes back intact", "[stringcompressor]" )
{
    StringCompressorScope compressor;

    CHECK( Decode( "The qui", 8 ) == "The qui" );
}

TEST_CASE( "A std::string decode does not ask rakMalloc_Ex for maxCharsToWrite bytes", "[stringcompressor]" )
{
    // The old decode took a maxCharsToWrite block from rakMalloc_Ex. The new buffer is the
    // std::string itself, which the standard allocator serves, so this probe cannot measure
    // it: it pins only that no RakNet allocation is sized by the caller's maximum.
    StringCompressorScope compressor;

    RakNet::BitStream bitStream;
    RakNet::StringCompressor::Instance()->EncodeString( "hello", 0, &bitStream );
    // No more bits than the stream holds, so no more bytes than its bit count, plus one.
    const size_t bound = (size_t)bitStream.GetNumberOfBitsUsed() + 1;

    std::string received;
    {
        LargestMalloc probe;
        REQUIRE( RakNet::StringCompressor::Instance()->DecodeString( received, INT_MAX, &bitStream ) );
        CHECK( LargestMalloc::Largest() <= bound );
    }
    CHECK( received == "hello" );
}

TEST_CASE( "EncodeString writes the bytes its English Huffman table has always written", "[stringcompressor]" )
{
    // A peer decodes these bytes with its own tree, so a different encoding is a wire break.
    StringCompressorScope compressor;

    RakNet::BitStream bitStream;
    RakNet::StringCompressor::Instance()->EncodeString( "Hello, World! 0x7F~", 256, &bitStream );

    const std::vector<int> written( bitStream.GetData(), bitStream.GetData() + bitStream.GetNumberOfBytesUsed() );
    const std::vector<int> expected = { 0x00, 0x00, 0x00, 0x44, 0x3E, 0x04, 0xA5, 0x1A, 0xFA, 0xB9, 0x19,
                                        0x93, 0x95, 0xDF, 0x51, 0x54, 0xD5, 0x75, 0xAB, 0x2A, 0xDA, 0x80 };
    CHECK( written == expected );
}
