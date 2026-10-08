#pragma once

#include "Export.h"

#include <cstddef>
#include <vector>

namespace RakNet {

/// Bytes queued on a TCP stream: appended at the back, read from the front.
///
/// Growth that cannot be allocated is fatal (ADR-0004).
class RAK_DLL_EXPORT TCPByteBuffer
{
public:
    void Append( const char* data, size_t n );

    /// Bytes buffered and not yet read or consumed.
    size_t Size() const;

    /// Copies the first \a n bytes to \a out. False, with \a out untouched, if fewer are buffered.
    bool Peek( char* out, size_t n ) const;

    /// As Peek, and consumes them.
    bool Read( char* out, size_t n );

    /// Drops the first \a n bytes. \a n must not exceed Size.
    void Consume( size_t n );

    /// Every buffered byte, in order, in one run of \a *n bytes. Valid until the next
    /// non-const call.
    const char* Contiguous( size_t* n ) const;

    void Clear();

private:
    std::vector<char> bytes;
    size_t readOffset = 0;
};

} // namespace RakNet
