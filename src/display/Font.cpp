#include "aw/display/Font.hpp"

#include <array>

namespace aw::display {
namespace {

constexpr std::uint8_t kSmallWidth = 6;
constexpr std::uint8_t kSmallHeight = 8;
constexpr std::uint8_t kLargeWidth = 12;
constexpr std::uint8_t kLargeHeight = 16;

#include "GeneratedFonts.inc"

const Font kSmall{kSmallWidth, kSmallHeight, 32, 95, kSmallGlyphs};
const Font kLarge{kLargeWidth, kLargeHeight, 32, 95, kLargeGlyphs};

}  // namespace

int Font::glyphOffset(char codePoint) const noexcept {
    const auto value = static_cast<unsigned char>(codePoint);
    if (value < firstCodePoint || value >= firstCodePoint + glyphCount) {
        return -1;
    }
    return static_cast<int>((value - firstCodePoint) * bytesPerGlyph());
}

std::int16_t Font::measure(std::string_view text) const noexcept {
    return static_cast<std::int16_t>(text.size() * width);
}

const Font& smallFont() noexcept { return kSmall; }
const Font& largeFont() noexcept { return kLarge; }

}  // namespace aw::display
