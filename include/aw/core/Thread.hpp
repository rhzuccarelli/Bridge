#pragma once

#include <cstdint>
#include <functional>
#include <thread>

namespace aw::core {

/// What a new thread does, for platforms that place threads on CPU cores.
enum class ThreadRole : std::uint8_t {
    General,  ///< Mostly waits: I/O, events. Runs wherever it is scheduled.
    Compute,  ///< Keeps a core busy for a long stretch, e.g. rendering an export.
};

/// Places the next thread the calling thread starts. Installed once at startup
/// by the platform, like the log sink; none by default, so every thread is a
/// plain `std::thread`.
///
/// On the ESP32-P4, a task that uses the FPU stays on the core it first ran
/// on. A compute thread that lands on the core the USB tasks use takes it
/// whole, so the platform puts compute threads on the other core.
using ThreadPlacer = void (*)(ThreadRole role);

namespace threads {

void installPlacer(ThreadPlacer placer) noexcept;

/// Starts `body` on a new thread placed for `role`.
[[nodiscard]] std::thread start(ThreadRole role, std::function<void()> body);

}  // namespace threads
}  // namespace aw::core
