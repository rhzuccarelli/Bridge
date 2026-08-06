#pragma once

#include <cstdint>

namespace aw::display {

/// A pixel, in the panel's native RGB565.
///
/// The whole rendering stack works in RGB565 rather than 24 bit colour and
/// converting at the end: the framebuffer is half the size, the blit is a
/// straight memcpy, and the panel cannot show more than this anyway.  The cost
/// is banding on gradients, which a UI made of flat rectangles and text does
/// not have.
using Color = std::uint16_t;

[[nodiscard]] constexpr Color rgb(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept {
    return static_cast<Color>(((r & 0xF8U) << 8) | ((g & 0xFCU) << 3) | (b >> 3));
}

/// The device's palette.
///
/// Fixed and small on purpose.  A hardware recorder that has to be readable in
/// a dark room and in daylight wants high contrast and few colours, and a
/// closed palette keeps every screen looking like the same device.
namespace palette {
inline constexpr Color kBackground = rgb(8, 8, 10);
inline constexpr Color kSurface = rgb(28, 28, 32);
inline constexpr Color kText = rgb(235, 235, 235);
inline constexpr Color kTextDim = rgb(140, 140, 145);
inline constexpr Color kAccent = rgb(255, 140, 0);      ///< Selection, focus.
inline constexpr Color kRecord = rgb(230, 40, 40);
inline constexpr Color kPlay = rgb(60, 200, 90);
inline constexpr Color kWaveform = rgb(120, 190, 255);
inline constexpr Color kMeterOk = rgb(60, 200, 90);
inline constexpr Color kMeterWarn = rgb(240, 190, 40);
inline constexpr Color kMeterClip = rgb(230, 40, 40);
}  // namespace palette

}  // namespace aw::display
