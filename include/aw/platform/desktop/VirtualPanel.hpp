#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "aw/core/Controls.hpp"
#include "aw/hal/DisplayInterface.hpp"

namespace aw::platform::desktop {

/// One touch target standing in for a physical button.
struct VirtualKey {
    core::ButtonId id{core::ButtonId::Play};
    hal::Rect area{};
    std::string_view label;
};

/// Geometry of the on-screen stand-in control panel.
///
/// This exists because the brief is explicit that the product has **no
/// touchscreen**, and that constraint must survive the prototype.  Rather than
/// teach the UI about touch, the panel turns a tap into an ordinary
/// `ButtonId` - so every event the application sees is byte-identical to one
/// produced by a real switch, and no touch idiom (drag, swipe, on-screen
/// scrolling) can creep into the interaction design.
///
/// Layout on a 480x320 panel: the 320x240 logical canvas sits centred along the
/// top, leaving a 480x80 strip underneath for two rows of six keys.  The canvas
/// is *not* stretched to fill the panel, because the shipping device has a
/// 320x240 screen and designing layouts against 480x320 would be rework.
///
/// Pure geometry with no I/O, so the hit testing is unit tested without a
/// touchscreen, a framebuffer, or a Raspberry Pi.
class VirtualPanelLayout {
public:
    /// `panel` is the physical screen size; `canvas` the logical UI size.
    VirtualPanelLayout(std::uint16_t panelWidth,
                       std::uint16_t panelHeight,
                       std::uint16_t canvasWidth,
                       std::uint16_t canvasHeight);

    /// Where the logical canvas sits within the physical panel.
    [[nodiscard]] hal::Rect canvasArea() const noexcept { return canvasArea_; }

    /// The strip containing the virtual keys.  Empty when the panel is too
    /// small to spare the room, in which case there are no keys either.
    [[nodiscard]] hal::Rect keyStripArea() const noexcept { return keyStripArea_; }

    [[nodiscard]] const std::vector<VirtualKey>& keys() const noexcept { return keys_; }

    /// Which key, if any, is under a physical panel coordinate.
    [[nodiscard]] std::optional<core::ButtonId> hitTest(std::int16_t x,
                                                        std::int16_t y) const noexcept;

private:
    hal::Rect canvasArea_{};
    hal::Rect keyStripArea_{};
    std::vector<VirtualKey> keys_;
};

}  // namespace aw::platform::desktop
