///
/// \brief \b [Internal] The operating system's random number source.
///

#pragma once

#include "Export.h"

#include <cstddef>

namespace RakNet {

/// \internal
/// Fills a buffer from the operating system's cryptographically secure random
/// number generator: BCryptGenRandom on Windows, /dev/urandom elsewhere.
///
/// This is the only entropy source for values that must be unique - see
/// docs/adr/0001-identifiers-draw-from-the-platform-csprng.md. Clock readings are
/// not an entropy source, and <random> engines are for simulation, not for naming a Peer.
///
/// Reports failure by return value and never throws, per
/// docs/adr/0002-raknet-does-not-use-exceptions.md. Note this rules out
/// std::random_device, whose only failure channel is an exception.
///
/// \param[out] buffer Buffer to fill. Its contents are unspecified when this returns
///            false - a failure may come after part of the buffer was written.
/// \param[in] bytes Number of bytes to write. Zero is a successful no-op. On Windows a
///            request above ULONG_MAX fails, which no caller comes near.
/// \return true on success. On false the caller must not use the buffer.
bool RAK_DLL_EXPORT FillRandomBytes( void* buffer, size_t bytes );

} // namespace RakNet
