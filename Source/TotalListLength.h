#pragma once

#include <algorithm>
#include <cstdint>
#include <numeric>

namespace RakNet {

/// The length of a SendList concatenation, which skips blocks with a non-positive length.
/// Summed in uint64_t, so no total of int lengths wraps to one under MAXIMUM_MESSAGE_SIZE.
inline uint64_t TotalListLength( const int* lengths, int numParameters )
{
    return std::accumulate( lengths, lengths + std::max<int>( numParameters, 0 ), uint64_t( 0 ), []( uint64_t total, int length ) { return length > 0 ? total + (uint64_t)length : total; } );
}

} // namespace RakNet
