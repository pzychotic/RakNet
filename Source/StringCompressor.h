/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

/// \file
/// \brief \b Compresses/Decompresses ASCII strings and writes/reads them to BitStream class instances.  You can use this to easily serialize and deserialize your own strings.
///

#pragma once

#include "Export.h"

#include <string>

namespace RakNet {

/// Forward declarations
class BitStream;
class HuffmanEncodingTree;

/// \brief Writes and reads strings to and from bitstreams.
///
/// Only works with ASCII strings.  The default compression is for English.
class RAK_DLL_EXPORT StringCompressor
{
public:
    // Destructor
    ~StringCompressor();

    /// static function because only static functions can access static members
    /// The RakPeer constructor adds a reference to this class, so don't call this until an instance of RakPeer exists, or unless you call AddReference yourself.
    /// \return the unique instance of the StringCompressor
    static StringCompressor* Instance( void );

    /// Writes input to output, compressed.  Takes care of the null terminator for you.
    /// \param[in] input Pointer to an ASCII string
    /// \param[in] maxCharsToWrite The max number of bytes to write of \a input.  Use 0 to mean no limit.
    /// \param[out] output The bitstream to write the compressed string to
    /// \param[in] languageID Which language to use
    void EncodeString( const char* input, int maxCharsToWrite, BitStream* output );

    /// Writes input to output, uncompressed.  Takes care of the null terminator for you.
    /// \param[out] output A block of bytes to receive the output
    /// \param[in] maxCharsToWrite Size, in bytes, of \a output .  A NULL terminator will always be appended to the output string.  If the maxCharsToWrite is not large enough, the string will be truncated.
    /// \param[in] input The bitstream containing the compressed string
    /// \param[in] languageID Which language to use
    bool DecodeString( char* output, int maxCharsToWrite, BitStream* input );

    void EncodeString( const std::string& input, int maxCharsToWrite, BitStream* output );
    bool DecodeString( std::string& output, int maxCharsToWrite, BitStream* input );

    /// Used so I can allocate and deallocate this singleton at runtime
    static void AddReference( void );

    /// Used so I can allocate and deallocate this singleton at runtime
    static void RemoveReference( void );

    StringCompressor();

private:
    /// Singleton instance
    static StringCompressor* instance;

    /// Pointer to the huffman encoding tree.
    HuffmanEncodingTree* m_pHuffmanEncodingTree;

    static int referenceCount;
};

} // namespace RakNet
