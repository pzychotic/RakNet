
#include "StringUtils.h"

namespace RakNet {

unsigned long hash( const std::string& key )
{
    uint32_t c = 0u;
    uint32_t hash = 0u;
    const char* str = key.c_str();

    while( ( c = *str++ ) )
    {
        hash = c + ( hash << 6 ) + ( hash << 16 ) - hash;
    }

    return hash;
}

} // namespace RakNet
