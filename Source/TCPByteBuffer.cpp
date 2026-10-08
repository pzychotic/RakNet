#include "TCPByteBuffer.h"
#include "RakNetDefines.h"

#include <cassert>
#include <cstring>

namespace RakNet {

void TCPByteBuffer::Append( const char* data, size_t n )
{
    bytes.insert( bytes.end(), data, data + n );
}

size_t TCPByteBuffer::Size() const
{
    return bytes.size() - readOffset;
}

bool TCPByteBuffer::Peek( char* out, size_t n ) const
{
    if( Size() < n )
        return false;
    if( n > 0 )
        memcpy( out, bytes.data() + readOffset, n );
    return true;
}

bool TCPByteBuffer::Read( char* out, size_t n )
{
    if( Peek( out, n ) == false )
        return false;
    Consume( n );
    return true;
}

void TCPByteBuffer::Consume( size_t n )
{
    RakAssert( n <= Size() );
    readOffset += n;
    // What is moved is never more than what was consumed since the last compaction.
    if( readOffset > bytes.size() / 2 )
    {
        bytes.erase( bytes.begin(), bytes.begin() + readOffset );
        readOffset = 0;
    }
}

const char* TCPByteBuffer::Contiguous( size_t* n ) const
{
    *n = Size();
    return bytes.data() + readOffset;
}

void TCPByteBuffer::Clear()
{
    std::vector<char>().swap( bytes );
    readOffset = 0;
}

} // namespace RakNet
