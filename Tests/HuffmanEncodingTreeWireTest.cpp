#include "BitStream.h"
#include "HuffmanEncodingTree.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <vector>

/*
The bytes HuffmanEncodingTree writes for tables with many equal weights. While building the
tree, a node goes into the sorted list before the nodes of equal weight, and that tie order
decides the tree's shape and so every code. A peer decodes with a tree built from the same
table, so a different shape is a wire break. The English table StringCompressor uses is
pinned in its own test.
*/

using namespace RakNet;

namespace {

std::vector<int> Encoded( unsigned int ( &frequencyTable )[256], const char* text )
{
    HuffmanEncodingTree tree;
    tree.GenerateFromFrequencyTable( frequencyTable );

    BitStream bs;
    tree.EncodeArray( (unsigned char*)text, strlen( text ), &bs );
    return std::vector<int>( bs.GetData(), bs.GetData() + bs.GetNumberOfBytesUsed() );
}

} // namespace

TEST_CASE( "A table of equal weights encodes as it always has", "[huffman]" )
{
    // Every code is eight bits; only the tie order decides which byte gets which.
    unsigned int frequencyTable[256];
    for( unsigned int& weight : frequencyTable )
        weight = 1;

    CHECK( Encoded( frequencyTable, "Hello, World! 0x7F~" ) == std::vector<int>{ 0x1D, 0x30, 0x39, 0x39, 0x3A, 0x79, 0x75, 0x02, 0x3A, 0x27,
                                                                                 0x39, 0x31, 0x74, 0x75, 0x65, 0x2D, 0x62, 0x13, 0x2B } );
}

TEST_CASE( "A table of three repeating weights encodes as it always has", "[huffman]" )
{
    // Ties among the leaves and among the merged nodes, with codes of several lengths.
    unsigned int frequencyTable[256];
    for( unsigned int i = 0; i < 256; ++i )
        frequencyTable[i] = 1 + i % 3;

    CHECK( Encoded( frequencyTable, "Hello, World! 0x7F~" ) == std::vector<int>{ 0xA0, 0xF1, 0xDD, 0x6E, 0xB7, 0x4F, 0x6D, 0xAA, 0x65, 0xD2, 0xEF,
                                                                                 0x75, 0x76, 0xA9, 0x6D, 0x56, 0x6E, 0x38, 0x39, 0x97, 0x3D } );
}
