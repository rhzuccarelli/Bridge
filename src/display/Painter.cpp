#include "aw/display/Painter.hpp"

#include <algorithm>
#include <cmath>

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

    if (right <= left || bottom <= top) {
        return;
    }

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

std::int16_t Painter::drawText(std::int16_t x,
                               std::int16_t y,
                               std::string_view text,
                               const Font& font,
                               Color color) noexcept {
    std::int16_t cursor = x;
    for (const char character : text) {
        int offset = font.glyphOffset(character);
        if (offset < 0) offset = font.glyphOffset('?');
        if (offset >= 0 && static_cast<std::size_t>(offset + font.bytesPerGlyph()) <=
                               font.glyphs.size()) {
            for (std::uint8_t row = 0; row < font.height; ++row) {
                for (std::uint8_t column = 0; column < font.width; ++column) {
                    const std::size_t byte = static_cast<std::size_t>(offset) +
                                             row * font.bytesPerRow() + column / 8U;
                    if ((font.glyphs[byte] & (0x80U >> (column % 8U))) != 0U) {
                        fillRect(hal::Rect{static_cast<std::int16_t>(cursor + column),
                                           static_cast<std::int16_t>(y + row), 1, 1}, color);
                    }
                }
            }
        }
        cursor = static_cast<std::int16_t>(cursor + font.width);
    }
    return font.measure(text);
}

void Painter::drawTextIn(const hal::Rect& area,
                         std::string_view text,
                         const Font& font,
                         Color color,
                         TextAlign align) noexcept {
    const std::int16_t width = font.measure(text);
    std::int16_t x = area.x;
    if (align == TextAlign::Center) x = static_cast<std::int16_t>(area.x + (area.width - width) / 2);
    if (align == TextAlign::Right) x = static_cast<std::int16_t>(area.x + area.width - width);
    const std::int16_t y = static_cast<std::int16_t>(area.y + (area.height - font.height) / 2);
    (void)drawText(x, y, text, font, color);
}

void Painter::drawWaveform(const hal::Rect& area,
                           std::span<const std::pair<float, float>> peaks,
                           Color color) noexcept {
    if (area.isEmpty() || peaks.empty()) return;
    const float half = static_cast<float>(area.height - 1) * 0.5F;
    const std::int16_t centre = static_cast<std::int16_t>(area.y + area.height / 2);
    for (std::int16_t x = 0; x < area.width; ++x) {
        const auto& peak = peaks[std::min<std::size_t>(
            static_cast<std::size_t>(x) * peaks.size() / static_cast<std::size_t>(area.width),
            peaks.size() - 1U)];
        const auto top = static_cast<std::int16_t>(centre - std::clamp(peak.second, -1.0F, 1.0F) * half);
        const auto bottom = static_cast<std::int16_t>(centre - std::clamp(peak.first, -1.0F, 1.0F) * half);
        drawVLine(static_cast<std::int16_t>(area.x + x), std::min(top, bottom),
                  static_cast<std::int16_t>(std::abs(bottom - top) + 1), color);
    }
}

void Painter::drawMeter(const hal::Rect& area, float level, bool vertical) noexcept {
    drawRect(area, palette::kTextDim);
    const float clamped = std::clamp(level, 0.0F, 1.0F);
    if (vertical) {
        const auto fill = static_cast<std::int16_t>((area.height - 2) * clamped);
        fillRect(hal::Rect{static_cast<std::int16_t>(area.x + 1),
                           static_cast<std::int16_t>(area.y + area.height - 1 - fill),
                           static_cast<std::int16_t>(area.width - 2), fill}, palette::kMeterOk);
    } else {
        const auto fill = static_cast<std::int16_t>((area.width - 2) * clamped);
        fillRect(hal::Rect{static_cast<std::int16_t>(area.x + 1),
                           static_cast<std::int16_t>(area.y + 1), fill,
                           static_cast<std::int16_t>(area.height - 2)}, palette::kMeterOk);
    }
}

}  // namespace aw::display
