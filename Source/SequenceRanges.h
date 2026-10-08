#pragma once

#include "BitStream.h"

#include <algorithm>
#include <vector>

namespace RakNet {

/// A set of sequence numbers held as ascending, disjoint, non-adjacent runs, as
/// ReliabilityLayer acknowledges datagrams or reports them missing.
///
/// On the wire: a byte-aligned uint16 count, then per run a byte that is 1 when the run is a
/// single number, the first number, and the last number only when it differs.
template<class T>
class SequenceRanges
{
public:
    struct Range
    {
        T first;
        T last;
    };

    /// Adds \a number, extending or joining the runs it touches.
    void Insert( T number );

    void Clear();

    /// Number of runs.
    unsigned Size() const;

    /// Number of sequence numbers across all runs.
    unsigned RangeSum() const;

    const std::vector<Range>& Ranges() const;

    /// Writes the leading runs whose budget fits \a maxBits and returns the bits budgeted.
    /// With \a clearSerialized, the runs written are removed and the rest are kept for the
    /// next datagram.
    BitSize_t Serialize( BitStream* out, BitSize_t maxBits, bool clearSerialized );

    /// Replaces the contents with the runs read from \a in. False on a short read or a run
    /// whose last number is below its first.
    bool Deserialize( BitStream* in );

private:
    std::vector<Range> ranges;
};

template<class T>
void SequenceRanges<T>::Insert( T number )
{
    auto next = std::upper_bound( ranges.begin(), ranges.end(), number, []( const T& n, const Range& range ) { return n < range.first; } );
    const bool joinsNext = next != ranges.end() && number + T( 1 ) == next->first;

    if( next != ranges.begin() )
    {
        Range& previous = *( next - 1 );
        if( !( previous.last < number ) )
            return;
        if( previous.last + T( 1 ) == number )
        {
            if( joinsNext )
            {
                previous.last = next->last;
                ranges.erase( next );
            }
            else
                previous.last = number;
            return;
        }
    }

    if( joinsNext )
        next->first = number;
    else
        ranges.insert( next, Range{ number, number } );
}

template<class T>
void SequenceRanges<T>::Clear()
{
    ranges.clear();
}

template<class T>
unsigned SequenceRanges<T>::Size() const
{
    return (unsigned)ranges.size();
}

template<class T>
unsigned SequenceRanges<T>::RangeSum() const
{
    unsigned sum = 0;
    for( const Range& range : ranges )
        sum += static_cast<unsigned>( range.last - range.first ) + 1;
    return sum;
}

template<class T>
const std::vector<typename SequenceRanges<T>::Range>& SequenceRanges<T>::Ranges() const
{
    return ranges;
}

template<class T>
BitSize_t SequenceRanges<T>::Serialize( BitStream* out, BitSize_t maxBits, bool clearSerialized )
{
    // Each number is budgeted at sizeof( T ) bytes, as RakNet 4.x does, so for uint24_t the
    // budget overstates the three bytes written and datagrams are cut where 4.x cuts them.
    const BitSize_t countBits = sizeof( unsigned short ) * 8;
    const BitSize_t numberBits = sizeof( T ) * 8;

    BitStream runs;
    BitSize_t bitsWritten = 0;
    unsigned short countWritten = 0;
    for( const Range& range : ranges )
    {
        if( countWritten == (unsigned short)-1 || countBits + bitsWritten + numberBits * 2 + 1 > maxBits )
            break;
        const unsigned char single = range.first == range.last ? 1 : 0;
        runs.Write( single );
        runs.Write( range.first );
        bitsWritten += numberBits + 8;
        if( !single )
        {
            runs.Write( range.last );
            bitsWritten += numberBits;
        }
        countWritten++;
    }

    out->AlignWriteToByteBoundary();
    const BitSize_t before = out->GetWriteOffset();
    out->Write( countWritten );
    bitsWritten += out->GetWriteOffset() - before;
    out->Write( &runs, runs.GetNumberOfBitsUsed() );

    if( clearSerialized )
        ranges.erase( ranges.begin(), ranges.begin() + countWritten );

    return bitsWritten;
}

template<class T>
bool SequenceRanges<T>::Deserialize( BitStream* in )
{
    ranges.clear();
    unsigned short count;
    in->AlignReadToByteBoundary();
    if( !in->Read( count ) )
        return false;

    for( unsigned short i = 0; i < count; i++ )
    {
        unsigned char single;
        Range range;
        if( !in->Read( single ) || !in->Read( range.first ) )
            return false;
        if( single )
            range.last = range.first;
        else if( !in->Read( range.last ) || range.last < range.first )
            return false;
        ranges.push_back( range );
    }
    return true;
}

} // namespace RakNet
