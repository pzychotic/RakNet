#include "PlatformRandom.h"

#if defined( _WIN32 )
#include <windows.h>
#include <bcrypt.h>
#include <cstdint>
#else
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace RakNet {

#if defined( _WIN32 )

bool FillRandomBytes( void* buffer, size_t bytes )
{
    if( bytes == 0 )
        return true;
    if( buffer == 0 )
        return false;

#if SIZE_MAX > 0xFFFFFFFFu
    // BCryptGenRandom takes a ULONG count. Library callers draw eight bytes and the
    // largest draw in the tree is a test's eight megabytes, so a request this large is a
    // caller bug; report it rather than carry a chunking loop nothing reaches.
    if( bytes > 0xFFFFFFFFu )
        return false;
#endif

    // BCRYPT_USE_SYSTEM_PREFERRED_RNG means no algorithm handle is needed, which is what
    // keeps this to a single call.
    const NTSTATUS status = BCryptGenRandom( NULL, (PUCHAR)buffer, (ULONG)bytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG );
    return BCRYPT_SUCCESS( status );
}

#else

bool FillRandomBytes( void* buffer, size_t bytes )
{
    if( bytes == 0 )
        return true;
    if( buffer == 0 )
        return false;

    // Opened per call rather than cached in a static: no thread safety to get wrong,
    // and no descriptor to leak into a child across fork(). One open costs microseconds
    // against the ~240 ms this function exists to remove, and callers draw once per peer.
    //
    // getrandom(2) or arc4random_buf(3) would avoid the descriptor entirely and would be
    // worth the extra platform branches if RakNet ever had to draw before /dev/urandom is
    // mounted, or under descriptor exhaustion. Neither applies to a userspace application
    // constructing a network peer, so this stays one branch.
    const int fd = open( "/dev/urandom", O_RDONLY );
    if( fd < 0 )
        return false;

    unsigned char* pCursor = (unsigned char*)buffer;
    size_t remaining = bytes;
    while( remaining > 0 )
    {
        const ssize_t got = read( fd, pCursor, remaining );
        if( got < 0 )
        {
            if( errno == EINTR )
                continue;

            close( fd );
            return false;
        }
        if( got == 0 )
        {
            // /dev/urandom never signals end of file. If it did, something is wrong
            // enough that looping would spin forever.
            close( fd );
            return false;
        }

        pCursor += got;
        remaining -= (size_t)got;
    }

    close( fd );
    return true;
}

#endif

} // namespace RakNet
