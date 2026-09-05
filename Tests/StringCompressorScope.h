#pragma once

#include "StringCompressor.h"

/*
 *  Holds a reference to the StringCompressor singleton for the lifetime of a
 *  Catch2 test case, and releases it on the way out - normally, or by exception.
 *
 *  StringCompressor is reference counted and only built on the first
 *  AddReference(); Instance() returns null until then. RakPeer's constructor is
 *  what normally takes that reference, so any test that reaches the compressed
 *  BitStream path (WriteCompressed / ReadCompressed on a std::string, which is
 *  Huffman coded through StringCompressor) without constructing a peer has to
 *  take one itself.
 *
 *  Declared as a plain local, first line of the test body, for the same reasons
 *  PeerScope is:
 *
 *      TEST_CASE( "...", "[bitstream]" )
 *      {
 *          StringCompressorScope compressor;
 *          ...
 *      }
 */
class StringCompressorScope
{
public:
    StringCompressorScope() { RakNet::StringCompressor::AddReference(); }
    ~StringCompressorScope() { RakNet::StringCompressor::RemoveReference(); }

    StringCompressorScope( const StringCompressorScope& ) = delete;
    StringCompressorScope& operator=( const StringCompressorScope& ) = delete;
};
