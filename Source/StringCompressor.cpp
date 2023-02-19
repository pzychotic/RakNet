/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "StringCompressor.h"
#include "DS_HuffmanEncodingTree.h"
#include "BitStream.h"
#include "RakAssert.h"
#include "RakMemoryOverride.h"

#include <stdint.h>
#include <string.h>

namespace RakNet {

StringCompressor* StringCompressor::instance = 0;
int StringCompressor::referenceCount = 0;

void StringCompressor::AddReference( void )
{
    if( ++referenceCount == 1 )
    {
        instance = RakNet::OP_NEW<StringCompressor>( _FILE_AND_LINE_ );
    }
}
void StringCompressor::RemoveReference( void )
{
    RakAssert( referenceCount > 0 );

    if( referenceCount > 0 )
    {
        if( --referenceCount == 0 )
        {
            RakNet::OP_DELETE( instance, _FILE_AND_LINE_ );
            instance = 0;
        }
    }
}

StringCompressor* StringCompressor::Instance( void )
{
    return instance;
}

// clang-format off
unsigned int englishCharacterFrequencies[256] = {    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,  722,    0,    0,    2,    0,    0,
                                                     0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
                                                 11084,   58,   63,    1,    0,   31,    0,  317,   64,   64,   44,    0,  695,   62,  980,  266,
                                                    69,   67,   56,    7,   73,    3,   14,    2,   69,    1,  167,    9,    1,    2,   25,   94,
                                                     0,  195,  139,   34,   96,   48,  103,   56,  125,  653,   21,    5,   23,   64,   85,   44,
                                                    34,    7,   92,   76,  147,   12,   14,   57,   15,   39,   15,    1,    1,    1,    2,    3,
                                                     0, 3611,  845, 1077, 1884, 5870,  841, 1057, 2501, 3212,  164,  531, 2019, 1330, 3056, 4037,
                                                   848,   47, 2586, 2919, 4771, 1707,  535, 1106,  152, 1243,  100,    0,    2,    0,   10,    0,
                                                     0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
                                                     0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
                                                     0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
                                                     0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
                                                     0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
                                                     0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
                                                     0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
                                                     0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0 };
// clang-format on

StringCompressor::StringCompressor()
{
    // Make a default tree immediately, since this is used for RPC possibly from multiple threads at the same time
    m_pHuffmanEncodingTree = RakNet::OP_NEW<HuffmanEncodingTree>( _FILE_AND_LINE_ );
    m_pHuffmanEncodingTree->GenerateFromFrequencyTable( englishCharacterFrequencies );
}

StringCompressor::~StringCompressor()
{
    RakNet::OP_DELETE( m_pHuffmanEncodingTree, _FILE_AND_LINE_ );
}

void StringCompressor::EncodeString( const char* input, int maxCharsToWrite, BitStream* output )
{
    if( input == 0 )
    {
        output->WriteCompressed( (uint32_t)0 );
        return;
    }

    BitStream encodedBitStream;

    int charsToWrite;

    if( maxCharsToWrite <= 0 || (int)strlen( input ) < maxCharsToWrite )
        charsToWrite = (int)strlen( input );
    else
        charsToWrite = maxCharsToWrite - 1;

    m_pHuffmanEncodingTree->EncodeArray( (unsigned char*)input, charsToWrite, &encodedBitStream );

    uint32_t stringBitLength = (uint32_t)encodedBitStream.GetNumberOfBitsUsed();

    output->WriteCompressed( stringBitLength );

    output->WriteBits( encodedBitStream.GetData(), stringBitLength );
}

bool StringCompressor::DecodeString( char* output, int maxCharsToWrite, BitStream* input )
{
    if( maxCharsToWrite <= 0 )
        return false;

    uint32_t stringBitLength;

    output[0] = 0;

    if( input->ReadCompressed( stringBitLength ) == false )
        return false;

    if( (unsigned)input->GetNumberOfUnreadBits() < stringBitLength )
        return false;

    int bytesInStream = m_pHuffmanEncodingTree->DecodeArray( input, stringBitLength, maxCharsToWrite, (unsigned char*)output );

    if( bytesInStream < maxCharsToWrite )
        output[bytesInStream] = 0;
    else
        output[maxCharsToWrite - 1] = 0;

    return true;
}

void StringCompressor::EncodeString( const std::string& input, int maxCharsToWrite, BitStream* output )
{
    EncodeString( input.c_str(), maxCharsToWrite, output );
}
bool StringCompressor::DecodeString( std::string& output, int maxCharsToWrite, BitStream* input )
{
    if( maxCharsToWrite <= 0 )
    {
        output.clear();
        return true;
    }

    bool out;

#if USE_ALLOCA == 1
    if( maxCharsToWrite < MAX_ALLOCA_STACK_ALLOCATION )
    {
        char* destinationBlock = (char*)alloca( maxCharsToWrite );
        out = DecodeString( destinationBlock, maxCharsToWrite, input );
        output = destinationBlock;
    }
    else
#endif
    {
        char* destinationBlock = (char*)rakMalloc_Ex( maxCharsToWrite, _FILE_AND_LINE_ );
        out = DecodeString( destinationBlock, maxCharsToWrite, input );
        output = destinationBlock;
        rakFree_Ex( destinationBlock, _FILE_AND_LINE_ );
    }

    return out;
}

} // namespace RakNet
