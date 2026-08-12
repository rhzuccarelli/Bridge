#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace aw::display {

/// A fixed-pitch bitmap font.
///
/// Bitmap rather than vector/TTF: no rasteriser, no hinting, no floating point,
/// no allocation, and glyphs land on exact pixels - which is what makes small
/// text readable on a 320x240 panel.  Two sizes cover the whole UI: a small one
/// for lists and labels, a large one for the transport readout you need to read
/// from across a room.
///
/// `glyphs` holds `glyphCount` glyphs of `bytesPerGlyph` bytes each, starting at
/// `firstCodePoint`.  Rows are top to bottom, MSB is the leftmost pixel.
/// Coverage is ASCII 32..126; anything else renders as the fallback glyph.
struct Font {
    std::uint8_t width{0};
    std::uint8_t height{0};
    std::uint8_t firstCodePoint{32};
    std::uint16_t glyphCount{95};
    std::span<const std::uint8_t> glyphs;

    [[nodiscard]] constexpr std::uint8_t bytesPerRow() const noexcept {
        return static_cast<std::uint8_t>((width + 7) / 8);
    }
    [[nodiscard]] constexpr std::uint16_t bytesPerGlyph() const noexcept {
        return static_cast<std::uint16_t>(bytesPerRow() * height);
    }

    /// Byte offset of `codePoint`'s bitmap, or -1 when it is not covered.
    [[nodiscard]] int glyphOffset(char codePoint) const noexcept;

    [[nodiscard]] std::int16_t measure(std::string_view text) const noexcept;
};

/// The two fonts the UI uses.  Phase 7 fills in the glyph data (generated into
/// `assets/fonts` and compiled in - the device must boot with no file reads).
[[nodiscard]] const Font& smallFont() noexcept;  ///< 8x12, OLED labels and lists.
[[nodiscard]] const Font& largeFont() noexcept;  ///< 12x16, timecode.

}  // namespace aw::display
