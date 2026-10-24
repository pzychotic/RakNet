#pragma once

#include <algorithm>
#include <string_view>

namespace RakNet {

namespace AsciiCase {

inline char ToLower( char c )
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>( c - 'A' + 'a' ) : c;
}

} // namespace AsciiCase

/// Whether \a lhs and \a rhs differ at most in the case of ASCII letters. Every other byte,
/// including one past 0x7F, compares as it is.
inline bool EqualsIgnoringAsciiCase( std::string_view lhs, std::string_view rhs )
{
    return lhs.size() == rhs.size() && std::equal( lhs.begin(), lhs.end(), rhs.begin(), []( char l, char r ) { return AsciiCase::ToLower( l ) == AsciiCase::ToLower( r ); } );
}

/// Whether \a lhs sorts before \a rhs, comparing unsigned bytes with ASCII letters lower-cased.
/// Neither of two strings EqualsIgnoringAsciiCase calls equal sorts before the other.
inline bool LessIgnoringAsciiCase( std::string_view lhs, std::string_view rhs )
{
    return std::lexicographical_compare( lhs.begin(), lhs.end(), rhs.begin(), rhs.end(), []( char l, char r ) { return static_cast<unsigned char>( AsciiCase::ToLower( l ) ) < static_cast<unsigned char>( AsciiCase::ToLower( r ) ); } );
}

} // namespace RakNet
