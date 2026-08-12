#pragma once

#include <cstdint>
#include <string_view>

namespace aw::core {

/// The complete physical control surface.
///
/// This enum lives in `core` rather than in `hal` or `events` because both of
/// those layers need it and neither may depend on the other.  It is closed by
/// design: the front panel is fixed hardware, so a new value here means a new
/// revision of the device, and every switch over it should be exhaustive.
enum class ButtonId : std::uint8_t {
    Play,
    Stop,
    Record,
    Import,
    Export,
    Undo,
    Save,
    Back,
    Track1,
    Track2,
    Track3,
    Track4,
    Track5,
    Track6,
    EncoderPush,

    Count  ///< Sentinel; also the size of any per-button array.
};

inline constexpr std::size_t kButtonCount = static_cast<std::size_t>(ButtonId::Count);

/// What happened to a button.
///
/// Debouncing and long-press detection happen in the HAL implementation, so
/// application code never sees switch bounce and never has to run timers.
enum class ButtonAction : std::uint8_t {
    Pressed,
    Released,
    LongPressed,  ///< Emitted once, while still held.
    Repeat,       ///< Auto-repeat while held, for value nudging.
};

[[nodiscard]] std::string_view toString(ButtonId id) noexcept;
[[nodiscard]] std::string_view toString(ButtonAction action) noexcept;

}  // namespace aw::core
