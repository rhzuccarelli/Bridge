#include "aw/display/Framebuffer.hpp"

#include <algorithm>

namespace aw::display {

Framebuffer::Framebuffer(std::uint16_t width, std::uint16_t height)
    : width_{width},
      height_{height},
      pixels_(static_cast<std::size_t>(width) * height, palette::kBackground) {
    // A new buffer is entirely undrawn, so the first flush must send everything.
    markDamaged(hal::Rect{0, 0, static_cast<std::int16_t>(width),
                          static_cast<std::int16_t>(height)});
}

void Framebuffer::setPixel(std::int16_t x, std::int16_t y, Color color) noexcept {
    if (x < 0 || y < 0 || x >= static_cast<std::int16_t>(width_) ||
        y >= static_cast<std::int16_t>(height_)) {
        return;
    }
    pixels_[static_cast<std::size_t>(y) * width_ + static_cast<std::size_t>(x)] = color;
    markDamaged(hal::Rect{x, y, 1, 1});
}

void Framebuffer::fill(Color color) noexcept {
    std::fill(pixels_.begin(), pixels_.end(), color);
    markDamaged(hal::Rect{0, 0, static_cast<std::int16_t>(width_),
                          static_cast<std::int16_t>(height_)});
}

std::span<Color> Framebuffer::row(std::uint16_t y) noexcept {
    if (y >= height_) {
        return {};
    }
    return std::span<Color>{pixels_.data() + static_cast<std::size_t>(y) * width_, width_};
}

void Framebuffer::markDamaged(const hal::Rect& area) noexcept {
    if (area.isEmpty()) {
        return;
    }

    // Clip to the buffer before merging, so damage can never describe pixels
    // that do not exist.
    const std::int16_t left = std::max<std::int16_t>(area.x, 0);
    const std::int16_t top = std::max<std::int16_t>(area.y, 0);
    const std::int16_t right =
        std::min<std::int16_t>(static_cast<std::int16_t>(area.x + area.width),
                               static_cast<std::int16_t>(width_));
    const std::int16_t bottom =
        std::min<std::int16_t>(static_cast<std::int16_t>(area.y + area.height),
                               static_cast<std::int16_t>(height_));

    if (right <= left || bottom <= top) {
        return;
    }

    if (damage_.isEmpty()) {
        damage_ = hal::Rect{left, top, static_cast<std::int16_t>(right - left),
                            static_cast<std::int16_t>(bottom - top)};
        return;
    }

    const std::int16_t mergedLeft = std::min(damage_.x, left);
    const std::int16_t mergedTop = std::min(damage_.y, top);
    const std::int16_t mergedRight =
        std::max(static_cast<std::int16_t>(damage_.x + damage_.width), right);
    const std::int16_t mergedBottom =
        std::max(static_cast<std::int16_t>(damage_.y + damage_.height), bottom);

    damage_ = hal::Rect{mergedLeft, mergedTop,
                        static_cast<std::int16_t>(mergedRight - mergedLeft),
                        static_cast<std::int16_t>(mergedBottom - mergedTop)};
}

void Framebuffer::clearDamage() noexcept { damage_ = hal::Rect{}; }

hal::Rect Framebuffer::extractDamage(std::span<Color> destination) const {
    if (damage_.isEmpty()) {
        return damage_;
    }

    const auto needed = static_cast<std::size_t>(damage_.pixelCount());
    if (destination.size() < needed) {
        return hal::Rect{};
    }

    for (std::int16_t y = 0; y < damage_.height; ++y) {
        const std::size_t sourceOffset =
            static_cast<std::size_t>(damage_.y + y) * width_ + static_cast<std::size_t>(damage_.x);
        const std::size_t destinationOffset =
            static_cast<std::size_t>(y) * static_cast<std::size_t>(damage_.width);
        std::copy_n(pixels_.begin() + static_cast<std::ptrdiff_t>(sourceOffset), damage_.width,
                    destination.begin() + static_cast<std::ptrdiff_t>(destinationOffset));
    }
    return damage_;
}

}  // namespace aw::display
