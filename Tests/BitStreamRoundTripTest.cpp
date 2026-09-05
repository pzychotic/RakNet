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

/*
The rest of the raw-buffer family: WriteCompressed, Read and ReadCompressed.

Same hazard as Write, and until recently a weaker guard: these carried
declared-but-undefined explicit specializations rather than deleted overloads, so
the spellings they happened to name failed at link time with a mangled symbol, and
the spellings they missed - WriteCompressed( charPtr ), WriteCompressed( "literal" ),
Read( constCharPtr ), ReadCompressed( wcharPtr ) and the wide ones - compiled,
linked, and mis-encoded. They are deleted overloads now, covered below the same way
Write is: as a viability trait, since there is still no negative-compilation harness.

The read side carries its own tag type. A deleted Read whose signature advises
Write_a_std_string_instead is wrong advice; the caller wants Read( std::string& ).

The entry points that forward rather than encode - WriteDelta, Serialize,
WriteCompressedDelta, SerializeCompressed, ReadDelta, ReadCompressedDelta - are
*not* asserted here, and deliberately so. They fail inside the template body rather
than in the immediate context, so deduction still succeeds and a viability trait
reports them callable; only a real call is ill-formed. Each was checked by hand
against the guard it forwards to:

    bs.WriteDelta( charPtr, charPtr );           // -> deleted Write( char*, ... )
    bs.Serialize( true, charPtr );               // -> deleted Write( char*, ... )
    bs.WriteCompressedDelta( charPtr, charPtr ); // -> deleted WriteCompressed( char*, ... )
    bs.SerializeCompressed( true, charPtr );     // -> deleted WriteCompressed / ReadCompressed
    bs.ReadDelta( charPtr );                     // -> deleted Read( char*&, ... )
    bs.ReadCompressedDelta( charPtr );           // -> deleted ReadCompressed( char*&, ... )

All six are compile errors through the guard they forward to, so none needs its own
deleted overload. The cost is that the diagnostic is reported inside BitStream.h
rather than at the call site - worse advice, but not a mis-encoding hazard, which is
the thing the deletions exist to prevent.

The directly-guarded calls, which do have to stay commented out:

    bs.WriteCompressed( somecharptr );   // error: WriteCompressed(char *, Write_a_std_string_instead) is deleted
    bs.WriteCompressed( "literal" );     // error: WriteCompressed(const char *, Write_a_std_string_instead) is deleted
    bs.Read( somecharptr );              // error: Read(char *&, Read_a_std_string_instead) is deleted
    bs.ReadCompressed( somecharptr );    // error: ReadCompressed(char *&, Read_a_std_string_instead) is deleted
*/

namespace {

template<class T, class = void>
struct WriteCompressedIsCallable : std::false_type
{
};

template<class T>
struct WriteCompressedIsCallable<T, std::void_t<decltype( std::declval<BitStream&>().WriteCompressed( std::declval<T>() ) )>>
    : std::true_type
{
};

template<class T, class = void>
struct ReadIsCallable : std::false_type
{
};

template<class T>
struct ReadIsCallable<T, std::void_t<decltype( std::declval<BitStream&>().Read( std::declval<T&>() ) )>> : std::true_type
{
};

template<class T, class = void>
struct ReadCompressedIsCallable : std::false_type
{
};

template<class T>
struct ReadCompressedIsCallable<T, std::void_t<decltype( std::declval<BitStream&>().ReadCompressed( std::declval<T&>() ) )>>
    : std::true_type
{
};

// WriteCompressed. The non-const pointer spellings and the literal are the four
// that used to compile, link, and put a pointer value or raw code units on the wire.
static_assert( !WriteCompressedIsCallable<const char ( & )[6]>::value, "WriteCompressed( \"literal\" ) must not compile" );
static_assert( !WriteCompressedIsCallable<char ( & )[6]>::value, "WriteCompressed( charArray ) must not compile" );
static_assert( !WriteCompressedIsCallable<const unsigned char ( & )[6]>::value,
               "WriteCompressed( unsigned char array ) must not compile" );
static_assert( !WriteCompressedIsCallable<const wchar_t ( & )[6]>::value, "WriteCompressed( L\"literal\" ) must not compile" );
static_assert( !WriteCompressedIsCallable<char*>::value, "WriteCompressed( char* ) must not compile" );
static_assert( !WriteCompressedIsCallable<const char*>::value, "WriteCompressed( const char* ) must not compile" );
static_assert( !WriteCompressedIsCallable<unsigned char*>::value, "WriteCompressed( unsigned char* ) must not compile" );
static_assert( !WriteCompressedIsCallable<const unsigned char*>::value,
               "WriteCompressed( const unsigned char* ) must not compile" );
static_assert( !WriteCompressedIsCallable<wchar_t*>::value, "WriteCompressed( wchar_t* ) must not compile" );
static_assert( !WriteCompressedIsCallable<const wchar_t*>::value, "WriteCompressed( const wchar_t* ) must not compile" );

static_assert( WriteCompressedIsCallable<std::string>::value,
               "WriteCompressed( std::string ) is the replacement and must compile" );
static_assert( WriteCompressedIsCallable<uint32_t>::value, "WriteCompressed( integral ) must be unaffected" );

// Read. Every one of these reads over the pointer variable rather than into the
// buffer, so all of them are rejected; const char*& and wchar_t*& are the two that
// the old specializations missed.
static_assert( !ReadIsCallable<char*>::value, "Read( char*& ) must not compile" );
static_assert( !ReadIsCallable<const char*>::value, "Read( const char*& ) must not compile" );
static_assert( !ReadIsCallable<unsigned char*>::value, "Read( unsigned char*& ) must not compile" );
static_assert( !ReadIsCallable<const unsigned char*>::value, "Read( const unsigned char*& ) must not compile" );
static_assert( !ReadIsCallable<wchar_t*>::value, "Read( wchar_t*& ) must not compile" );
static_assert( !ReadIsCallable<const wchar_t*>::value, "Read( const wchar_t*& ) must not compile" );

static_assert( ReadIsCallable<std::string>::value, "Read( std::string& ) is the replacement and must compile" );
static_assert( ReadIsCallable<uint32_t>::value, "Read( integral& ) must be unaffected" );

// ReadCompressed, likewise.
static_assert( !ReadCompressedIsCallable<char*>::value, "ReadCompressed( char*& ) must not compile" );
static_assert( !ReadCompressedIsCallable<const char*>::value, "ReadCompressed( const char*& ) must not compile" );
static_assert( !ReadCompressedIsCallable<unsigned char*>::value, "ReadCompressed( unsigned char*& ) must not compile" );
static_assert( !ReadCompressedIsCallable<const unsigned char*>::value,
               "ReadCompressed( const unsigned char*& ) must not compile" );
static_assert( !ReadCompressedIsCallable<wchar_t*>::value, "ReadCompressed( wchar_t*& ) must not compile" );
static_assert( !ReadCompressedIsCallable<const wchar_t*>::value, "ReadCompressed( const wchar_t*& ) must not compile" );

static_assert( ReadCompressedIsCallable<std::string>::value,
               "ReadCompressed( std::string& ) is the replacement and must compile" );
static_assert( ReadCompressedIsCallable<uint32_t>::value, "ReadCompressed( integral& ) must be unaffected" );

} // namespace
