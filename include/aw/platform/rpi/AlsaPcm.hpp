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

inline constexpr std::size_t kTp7MonitorChannels = 6;

struct Tp7MonitorConverterState {
    float previousLeft{0.0F};
    float previousRight{0.0F};
};

/// Converts one 48 kHz stereo block to the TP-7's native 96 kHz, six-channel
/// USB playback layout. FL/FR carry the monitor mix; all other channels are
/// silent. Linear interpolation avoids the strong imaging of sample repetition.
inline void convertStereo48ToTp7Monitor96(std::span<const float> stereo,
                                          std::span<float> tp7,
                                          Tp7MonitorConverterState& state) noexcept {
    const std::size_t inputFrames = stereo.size() / 2U;
    const std::size_t outputFrames = std::min(tp7.size() / kTp7MonitorChannels,
                                               inputFrames * 2U);
    std::fill(tp7.begin(), tp7.end(), 0.0F);
    for (std::size_t frame = 0; frame < inputFrames && frame * 2U < outputFrames; ++frame) {
        const float left = stereo[frame * 2U];
        const float right = stereo[frame * 2U + 1U];
        const std::size_t first = frame * 2U * kTp7MonitorChannels;
        tp7[first] = state.previousLeft;
        tp7[first + 1U] = state.previousRight;
        if (frame * 2U + 1U < outputFrames) {
            const std::size_t second = first + kTp7MonitorChannels;
            tp7[second] = (state.previousLeft + left) * 0.5F;
            tp7[second + 1U] = (state.previousRight + right) * 0.5F;
        }
        state.previousLeft = left;
        state.previousRight = right;
    }
}

}  // namespace aw::platform::rpi
