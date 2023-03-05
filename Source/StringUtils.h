
#pragma once

#include <string>

namespace RakNet {

template<typename ... Args>
std::string format( const char* pszFormat, Args ... args )
{
    std::string str( snprintf( nullptr, 0u, pszFormat, args... ), '\0' );
    snprintf( str.data(), str.size() + 1u, pszFormat, args... );
    return str;
}

} // namespace RakNet
