#include "StringUtils.h"

#include <cstdarg>
#include <cstdio>

namespace RakNet {

std::string format( const char* pszFormat, ... )
{
    va_list args;
    va_start( args, pszFormat );
    va_list measureArgs;
    va_copy( measureArgs, args );
    const int length = vsnprintf( nullptr, 0u, pszFormat, measureArgs );
    va_end( measureArgs );

    std::string str;
    if( length > 0 )
    {
        str.resize( static_cast<size_t>( length ) );
        if( vsnprintf( str.data(), str.size() + 1u, pszFormat, args ) < 0 )
            str.clear();
    }
    va_end( args );
    return str;
}

} // namespace RakNet
