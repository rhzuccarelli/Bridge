#pragma once

#include "aw/core/Result.hpp"
#include "aw/core/Types.hpp"
#include "aw/hal/Platform.hpp"

namespace aw::platform::simulator {

/// Builds a `hal::Platform` with no hardware behind it.
///
/// Why this exists at all, on a hardware project: it makes the whole
/// application runnable and debuggable on a laptop, which is where most of the
/// development actually happens.  It is also the proof that the HAL boundary is
/// real - if the app can run with every driver replaced, then no business logic
/// has leaked into a driver.
///
/// The audio output runs a wall-clock thread that calls the render callback at
/// the right rate and throws the samples away, so transport timing behaves like
/// the real thing.  The display renders into memory and can dump PPM frames.
[[nodiscard]] core::Result<hal::Platform> createPlatform(const core::Path& projectsRoot);

}  // namespace aw::platform::simulator
