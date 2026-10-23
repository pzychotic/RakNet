#pragma once

#include <algorithm>
#include <string_view>

namespace RakNet {

/// Whether \a lhs and \a rhs differ at most in the case of ASCII letters. Every other byte,
/// including one past 0x7F, compares as it is.
inline bool EqualsIgnoringAsciiCase( std::string_view lhs, std::string_view rhs )
{
    auto toLower = []( char c ) { return c >= 'A' && c <= 'Z' ? static_cast<char>( c - 'A' + 'a' ) : c; };
    return lhs.size() == rhs.size() && std::equal( lhs.begin(), lhs.end(), rhs.begin(), [&]( char l, char r ) { return toLower( l ) == toLower( r ); } );
}

} // namespace RakNet
