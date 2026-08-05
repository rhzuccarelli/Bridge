#include "aw/display/Font.hpp"

// Phase 7 generates the glyph data into assets/fonts and compiles it in.
// The empty spans here keep the API usable (measure() returns sane widths, so
// layout code can be written and tested) while nothing is drawn yet.

namespace aw::display {
namespace {

constexpr std::uint8_t kSmallWidth = 6;
constexpr std::uint8_t kSmallHeight = 10;
constexpr std::uint8_t kLargeWidth = 12;
constexpr std::uint8_t kLargeHeight = 20;

const Font kSmall{kSmallWidth, kSmallHeight, 32, 95, {}};
const Font kLarge{kLargeWidth, kLargeHeight, 32, 95, {}};

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
