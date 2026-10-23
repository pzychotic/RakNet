#pragma once

#include <charconv>
#include <string_view>

namespace RakNet {

/// Reads the whole of \a text as a decimal T into \a value. Returns false, leaving \a value
/// unchanged, for anything else: an empty string, whitespace, a '+', a '-' when T is
/// unsigned, trailing characters, or a value outside T's range.
template<typename T>
bool ParseWholeNumber( std::string_view text, T& value )
{
    const char* const end = text.data() + text.size();
    T parsed;
    auto res = std::from_chars( text.data(), end, parsed );
    if( res.ec != std::errc() || res.ptr != end )
        return false;
    value = parsed;
    return true;
}

} // namespace RakNet
