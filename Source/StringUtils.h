#pragma once

#include "Export.h"

#include <string>

/// Marks a printf-style function so GCC and Clang check its format string against
/// its arguments. \a fmt is the format parameter's 1-based position, \a first the
/// first variadic argument's.
#if defined( __GNUC__ ) || defined( __clang__ )
#define RAK_PRINTF_FORMAT( fmt, first ) __attribute__( ( format( printf, fmt, first ) ) )
#else
#define RAK_PRINTF_FORMAT( fmt, first )
#endif

namespace RakNet {

/// Formats as vsnprintf does and returns the whole output. Returns an empty string
/// when vsnprintf rejects the format or its arguments.
RAK_DLL_EXPORT std::string format( const char* pszFormat, ... ) RAK_PRINTF_FORMAT( 1, 2 );

} // namespace RakNet
