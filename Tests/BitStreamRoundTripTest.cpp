#include "BitStream.h"
#include "StringCompressorScope.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

/*
The shared BitStream round-trip harness.

Everything here writes fields into one BitStream and reads them back out of the
same stream, asserting on what comes out. That shape - write a sequence, read the
sequence, check the *later* fields - is what catches a framing bug, because a
serializer and a deserializer that disagree about padding produce a stream whose
first fields still read correctly and whose every subsequent field is garbage.
Checking only the field under test would pass.

Three ideas recur, and the RoundTrip helper below exists so the next ticket that
needs them does not hand-roll them again:

    aligned / unaligned start
        A BitStream field cares about the bit offset it begins at. Most in-tree
        call sites happen to write into a fresh stream at offset 0, which is byte
        aligned and hides alignment bugs. RoundTrip::Pad writes n throwaway bits
        first, so the field under test can be made to start at any of the eight
        offsets modulo a byte - and the aligned/unaligned pair below is one case
        run over GENERATE( 0 ... 7 ) rather than two hand-written ones.

    trailing sentinel
        kSentinel, written after the field under test. Its value is arbitrary;
        what matters is that a reader even one bit out of step cannot reproduce
        it. RoundTrip::WriteSentinel / CheckSentinel are the pair.

    reading back without copying
        The reader is a second BitStream over the writer's own buffer. BitStream
        is neither copyable nor movable - its copy constructor is private and
        asserts - so the reader cannot be returned by value and has to be built
        in place, which is what RoundTrip::Reader does. Storing both halves in
        one object also settles the lifetime: the buffer outlives the reader
        because the writer does.

This file is the home for BitStream serialization round trips generally, not just
the std::string ones below. Add to it rather than starting a sibling.

Tagged [bitstream] and deliberately not [network]: nothing here binds a socket or
constructs a peer, and the whole file should stay fast enough to run on every
save.
*/

using namespace RakNet;

namespace {

constexpr uint32_t kSentinel = 0xDEADBEEF;

// Long enough to span several bytes, so a byte-granular framing error shows up as
// a mangled string rather than a coincidentally equal one.
constexpr const char* kPayload = "the quick brown fox";

class RoundTrip
{
public:
    BitStream& Writer( void ) { return out; }

    // Pushes the next field written to bit offset `bits` modulo a byte. The bit
    // values are throwaway - they only have to be read back with the matching
    // ReadPad before the field under test.
    void Pad( int bits )
    {
        for( int i = 0; i < bits; ++i )
        {
            out.Write( true );
        }
    }

    void WriteSentinel( void ) { out.Write( kSentinel ); }

    // Reader over exactly the bytes written so far, sharing the writer's buffer
    // rather than copying it. The first call is what fixes the extent, so make it
    // once everything has been written; every later call hands back the same
    // reader, read offset and all, so `rt.Reader().Read( ... )` can be written
    // field by field without rewinding.
    BitStream& Reader( void )
    {
        if( !in )
        {
            in.emplace( out.GetData(), out.GetNumberOfBytesUsed(), false );
        }
        return *in;
    }

    void ReadPad( int bits )
    {
        for( int i = 0; i < bits; ++i )
        {
            bool bit = false;
            REQUIRE( Reader().Read( bit ) );
            CHECK( bit == true );
        }
    }

    void CheckSentinel( void )
    {
        uint32_t sentinel = 0;
        REQUIRE( Reader().Read( sentinel ) );
        CHECK( sentinel == kSentinel );
    }

private:
    BitStream out;
    std::optional<BitStream> in;
};

} // namespace

TEST_CASE( "empty std::string round trip preserves alignment for later fields", "[bitstream]" )
{
    // The regression this file was created for. BitStream::Serialize pads to the
    // byte boundary unconditionally - WriteAlignedBytes aligns before it looks at
    // the length - so an empty string still emits padding. Deserialize has to
    // consume that padding on the size == 0 path, because ReadAlignedBytes bails
    // out on a zero length *before* it reaches its own alignment step.
    //
    // Writer and reader disagree by up to 7 bits, so all seven unaligned starts
    // are exercised, not just the one a single leading bool produces. At pad 0 the
    // string is already byte aligned and there is nothing to get wrong; it is here
    // as the control.
    const int pad = GENERATE( 0, 1, 2, 3, 4, 5, 6, 7 );
    CAPTURE( pad );

    RoundTrip rt;
    rt.Pad( pad );
    rt.Writer().Write( std::string() );
    rt.WriteSentinel();

    rt.Reader();
    rt.ReadPad( pad );

    std::string str;
    REQUIRE( rt.Reader().Read( str ) );
    CHECK( str.empty() );

    rt.CheckSentinel();
}

TEST_CASE( "deserializing an empty std::string clears the destination", "[bitstream]" )
{
    // A caller reusing a std::string across reads - the normal way to deserialize
    // in a loop - must be able to tell "received empty" from "received nothing".
    // Without the clear on entry the previous value survives and the two are
    // indistinguishable.
    RoundTrip rt;
    rt.Writer().Write( std::string() );

    std::string str = "value from a previous read";
    REQUIRE( rt.Reader().Read( str ) );
    CHECK( str.empty() );
}

TEST_CASE( "non-empty std::string round trips at every starting offset", "[bitstream]" )
{
    const int pad = GENERATE( 0, 1, 2, 3, 4, 5, 6, 7 );
    CAPTURE( pad );

    RoundTrip rt;
    rt.Pad( pad );
    rt.Writer().Write( std::string( kPayload ) );
    rt.WriteSentinel();

    rt.Reader();
    rt.ReadPad( pad );

    std::string str;
    REQUIRE( rt.Reader().Read( str ) );
    CHECK( str == kPayload );

    rt.CheckSentinel();
}

TEST_CASE( "a truncated std::string read fails and leaves the destination empty", "[bitstream]" )
{
    // Length prefix claims more bytes than the stream holds. ReadAlignedBytes
    // returns false without touching the destination, so Deserialize has to clear
    // it itself - otherwise a failed read leaves the caller holding a buffer that
    // resize filled with NULs and that the caller has no reason to distrust.
    RoundTrip rt;
    rt.Writer().Write( static_cast<uint16_t>( 32 ) );
    rt.Writer().Write( static_cast<uint8_t>( 'x' ) );

    std::string str = "value from a previous read";
    CHECK_FALSE( rt.Reader().Read( str ) );
    CHECK( str.empty() );
}

TEST_CASE( "non-empty std::string round trips through WriteCompressed at every starting offset", "[bitstream]" )
{
    // The compressed path is Huffman coded, so it is bit granular throughout and
    // has no alignment step of its own to get wrong. The sentinel is still the
    // point: it fails unless the decoder stops on exactly the bit the encoder
    // stopped on, which is what MessageFilter's RPC4 handler depends on.
    StringCompressorScope compressor;

    const int pad = GENERATE( 0, 1, 2, 3, 4, 5, 6, 7 );
    CAPTURE( pad );

    RoundTrip rt;
    rt.Pad( pad );
    rt.Writer().WriteCompressed( std::string( kPayload ) );
    rt.WriteSentinel();

    rt.Reader();
    rt.ReadPad( pad );

    std::string str;
    REQUIRE( rt.Reader().ReadCompressed( str ) );
    CHECK( str == kPayload );

    rt.CheckSentinel();
}

TEST_CASE( "empty std::string round trips through WriteCompressed at every starting offset", "[bitstream]" )
{
    // Empty *and* unaligned is the combination that hid the uncompressed bug this
    // file was created for, so the compressed path gets the same sweep rather than
    // only the offset-zero case. The destination starts non-empty so a decoder that
    // writes nothing at all is told apart from one that correctly clears it.
    StringCompressorScope compressor;

    const int pad = GENERATE( 0, 1, 2, 3, 4, 5, 6, 7 );
    CAPTURE( pad );

    RoundTrip rt;
    rt.Pad( pad );
    rt.Writer().WriteCompressed( std::string() );
    rt.WriteSentinel();

    rt.Reader();
    rt.ReadPad( pad );

    std::string str = "value from a previous read";
    REQUIRE( rt.Reader().ReadCompressed( str ) );
    CHECK( str.empty() );

    rt.CheckSentinel();
}

/*
Raw character buffers and BitStream::Write.

There is no negative-compilation harness in this repo, so the deleted overloads
are covered the way a single translation unit can cover them: WriteIsCallable
below asks, in the immediate context of a template argument, whether
BitStream::Write( T ) is a viable call. Selecting a deleted overload makes that
expression ill-formed, so substitution fails and the trait is false - which turns
"this must not compile" into an assertion the normal build checks, without the
file itself failing to compile.

The corresponding real call sites, which do have to stay commented out:

    bs.Write( "literal" );      // error: Write(const char *, Write_a_std_string_instead) is deleted
    bs.Write( somecharptr );    // error: Write(char *, Write_a_std_string_instead) is deleted

Both used to compile, and both silently mis-encoded. Why they are deleted rather
than reinstated with std::string semantics, and what each one has to be rewritten
to, is on the deleted declarations in BitStream.h; it is not repeated here.
*/

namespace {

template<class T, class = void>
struct WriteIsCallable : std::false_type
{
};

template<class T>
struct WriteIsCallable<T, std::void_t<decltype( std::declval<BitStream&>().Write( std::declval<T>() ) )>> : std::true_type
{
};

// String literals, and any other array of characters.
static_assert( !WriteIsCallable<const char ( & )[6]>::value, "Write( \"literal\" ) must not compile" );
static_assert( !WriteIsCallable<char ( & )[6]>::value, "Write( charArray ) must not compile" );
static_assert( !WriteIsCallable<const unsigned char ( & )[6]>::value, "Write( unsigned char array ) must not compile" );

// The pointer spellings the upstream non-template overloads covered, wide ones
// included - those have no replacement here, which is the point of rejecting them.
static_assert( !WriteIsCallable<char*>::value, "Write( char* ) must not compile" );
static_assert( !WriteIsCallable<const char*>::value, "Write( const char* ) must not compile" );
static_assert( !WriteIsCallable<unsigned char*>::value, "Write( unsigned char* ) must not compile" );
static_assert( !WriteIsCallable<const unsigned char*>::value, "Write( const unsigned char* ) must not compile" );
static_assert( !WriteIsCallable<wchar_t*>::value, "Write( wchar_t* ) must not compile" );
static_assert( !WriteIsCallable<const wchar_t*>::value, "Write( const wchar_t* ) must not compile" );
static_assert( !WriteIsCallable<const wchar_t ( & )[6]>::value, "Write( L\"literal\" ) must not compile" );

// The replacement, and the unrelated types that must keep working. The bytes the
// replacement puts on the wire are covered by the round trips above; this only
// pins that the call is still viable.
static_assert( WriteIsCallable<std::string>::value, "Write( std::string ) is the replacement and must compile" );
static_assert( WriteIsCallable<uint32_t>::value, "Write( integral ) must be unaffected" );

} // namespace
