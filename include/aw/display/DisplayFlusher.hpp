#pragma once

#include <vector>

#include "aw/core/Result.hpp"
#include "aw/display/Framebuffer.hpp"
#include "aw/hal/DisplayInterface.hpp"

namespace aw::display {

/// Pushes a framebuffer's damaged region to the panel.
///
/// Separate from `Framebuffer` so the buffer stays a pure data structure that
/// tests can inspect without a display, and separate from `DisplayInterface` so
/// the damage policy is not duplicated in every backend.
class DisplayFlusher {
public:
    DisplayFlusher(hal::DisplayInterface& display, Framebuffer& framebuffer);

    DisplayFlusher(const DisplayFlusher&) = delete;
    DisplayFlusher& operator=(const DisplayFlusher&) = delete;

    /// Sends the damaged region, if any, and clears the damage.
    /// Cheap no-op when nothing changed - the main loop calls it every frame.
    [[nodiscard]] core::Status flush();

    /// Forces a full-screen send.  After a backlight change or a panel reset.
    [[nodiscard]] core::Status flushAll();

private:
    hal::DisplayInterface& display_;
    Framebuffer& framebuffer_;
    std::vector<Color> scratch_;  ///< Packed copy of the damaged region.
};

}  // namespace aw::display
