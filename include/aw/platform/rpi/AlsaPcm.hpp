#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace aw::platform::rpi {

/// Converts one normalized float sample to ALSA's packed signed 24-bit
/// little-endian representation. Kept ALSA-header-free for host-side tests.
inline void packS24_3LE(float value, std::span<std::byte, 3> destination) noexcept {
    const float clamped = std::clamp(value, -1.0F, 1.0F);
    const auto sample = static_cast<std::int32_t>(
        std::lrint(clamped >= 1.0F ? 8388607.0F : clamped * 8388608.0F));
    destination[0] = static_cast<std::byte>(sample & 0xFF);
    destination[1] = static_cast<std::byte>((sample >> 8) & 0xFF);
    destination[2] = static_cast<std::byte>((sample >> 16) & 0xFF);
}

}  // namespace aw::platform::rpi
