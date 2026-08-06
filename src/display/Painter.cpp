#include "aw/display/Painter.hpp"

#include <algorithm>

namespace aw::display {

void Painter::clear(Color color) noexcept { target_.fill(color); }

void Painter::fillRect(const hal::Rect& area, Color color) noexcept {
    if (area.isEmpty()) {
        return;
    }

    const std::int16_t left = std::max<std::int16_t>(area.x, 0);
    const std::int16_t top = std::max<std::int16_t>(area.y, 0);
    const std::int16_t right = std::min<std::int16_t>(
        static_cast<std::int16_t>(area.x + area.width),
        static_cast<std::int16_t>(target_.width()));
    const std::int16_t bottom = std::min<std::int16_t>(
        static_cast<std::int16_t>(area.y + area.height),
        static_cast<std::int16_t>(target_.height()));

    for (std::int16_t y = top; y < bottom; ++y) {
        std::span<Color> line = target_.row(static_cast<std::uint16_t>(y));
        std::fill(line.begin() + left, line.begin() + right, color);
    }

    target_.markDamaged(hal::Rect{left, top, static_cast<std::int16_t>(right - left),
                                  static_cast<std::int16_t>(bottom - top)});
}

void Painter::drawRect(const hal::Rect& area, Color color) noexcept {
    if (area.isEmpty()) {
        return;
    }
    drawHLine(area.x, area.y, area.width, color);
    drawHLine(area.x, static_cast<std::int16_t>(area.y + area.height - 1), area.width, color);
    drawVLine(area.x, area.y, area.height, color);
    drawVLine(static_cast<std::int16_t>(area.x + area.width - 1), area.y, area.height, color);
}

void Painter::drawHLine(std::int16_t x, std::int16_t y, std::int16_t length, Color color) noexcept {
    fillRect(hal::Rect{x, y, length, 1}, color);
}

void Painter::drawVLine(std::int16_t x, std::int16_t y, std::int16_t length, Color color) noexcept {
    fillRect(hal::Rect{x, y, 1, length}, color);
}

std::int16_t Painter::drawText(std::int16_t /*x*/,
                               std::int16_t /*y*/,
                               std::string_view text,
                               const Font& font,
                               Color /*color*/) noexcept {
    // Phase 7 blits glyph bitmaps.  The measured width is already correct, so
    // layout code written against this now will not move when glyphs arrive.
    return font.measure(text);
}

void Painter::drawTextIn(const hal::Rect& /*area*/,
                         std::string_view /*text*/,
                         const Font& /*font*/,
                         Color /*color*/,
                         TextAlign /*align*/) noexcept {
    // Phase 7.
}

void Painter::drawWaveform(const hal::Rect& /*area*/,
                           std::span<const std::pair<float, float>> /*peaks*/,
                           Color /*color*/) noexcept {
    // Phase 7.
}

void Painter::drawMeter(const hal::Rect& /*area*/, float /*level*/, bool /*vertical*/) noexcept {
    // Phase 7.
}

}  // namespace aw::display
