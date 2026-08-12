#include "aw/audio/SampleConverter.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace aw::audio::convert {
namespace {

constexpr float kInt16Scale = 32768.0F;
constexpr float kInt24Scale = 8388608.0F;
constexpr float kInt32Scale = 2147483648.0F;

template <typename T>
T readLittle(std::span<const std::byte> source, std::size_t offset) noexcept {
    std::array<std::byte, sizeof(T)> bytes{};
    std::copy_n(source.begin() + static_cast<std::ptrdiff_t>(offset), sizeof(T), bytes.begin());
    return std::bit_cast<T>(bytes);
}

template <typename T>
void writeLittle(T value, std::span<std::byte> destination, std::size_t offset) noexcept {
    const auto bytes = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
    std::copy(bytes.begin(), bytes.end(),
              destination.begin() + static_cast<std::ptrdiff_t>(offset));
}

float clampUnit(float value) noexcept { return std::clamp(value, -1.0F, 1.0F); }

}  // namespace

core::Status toFloat(std::span<const std::byte> source,
                     const core::AudioFormat& format,
                     std::span<std::span<float>> destination,
                     core::FrameCount frames) {
    if (frames < 0 || destination.size() != format.channels) {
        return core::makeError(core::ErrorCode::InvalidArgument,
                               "format/channel span mismatch");
    }

    const std::size_t frameCount = static_cast<std::size_t>(frames);
    for (core::ChannelCount channel = 0; channel < format.channels; ++channel) {
        if (destination[channel].size() < frameCount) {
            return core::makeError(core::ErrorCode::InvalidArgument,
                                   "destination channel too short");
        }
    }

    const std::size_t bytesPerFrame = format.bytesPerFrame();
    if (source.size() < frameCount * bytesPerFrame) {
        return core::makeError(core::ErrorCode::InvalidArgument, "source span too short");
    }

    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        const std::size_t frameOffset = frame * bytesPerFrame;
        for (core::ChannelCount channel = 0; channel < format.channels; ++channel) {
            const std::size_t offset =
                frameOffset + static_cast<std::size_t>(channel) *
                                  core::bytesPerSample(format.sampleFormat);
            float value = 0.0F;

            switch (format.sampleFormat) {
                case core::SampleFormat::Int16:
                    value = static_cast<float>(readLittle<std::int16_t>(source, offset)) /
                            kInt16Scale;
                    break;
                case core::SampleFormat::Int24:
                    value = int24ToFloat(source[offset], source[offset + 1], source[offset + 2]);
                    break;
                case core::SampleFormat::Int32:
                    value = static_cast<float>(readLittle<std::int32_t>(source, offset)) /
                            kInt32Scale;
                    break;
                case core::SampleFormat::Float32:
                    value = readLittle<float>(source, offset);
                    break;
            }

            destination[channel][frame] = clampUnit(value);
        }
    }

    return core::success();
}

core::Status fromFloat(std::span<const std::span<const float>> source,
                       const core::AudioFormat& format,
                       std::span<std::byte> destination,
                       core::FrameCount frames) {
    if (frames < 0 || source.size() != format.channels) {
        return core::makeError(core::ErrorCode::InvalidArgument, "format/channel span mismatch");
    }

    const std::size_t frameCount = static_cast<std::size_t>(frames);
    for (core::ChannelCount channel = 0; channel < format.channels; ++channel) {
        if (source[channel].size() < frameCount) {
            return core::makeError(core::ErrorCode::InvalidArgument, "source channel too short");
        }
    }

    const std::size_t bytesPerFrame = format.bytesPerFrame();
    if (destination.size() < frameCount * bytesPerFrame) {
        return core::makeError(core::ErrorCode::InvalidArgument,
                               "destination byte span too short");
    }

    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        const std::size_t frameOffset = frame * bytesPerFrame;
        for (core::ChannelCount channel = 0; channel < format.channels; ++channel) {
            const std::size_t offset =
                frameOffset + static_cast<std::size_t>(channel) *
                                  core::bytesPerSample(format.sampleFormat);
            const float value = clampUnit(source[channel][frame]);

            switch (format.sampleFormat) {
                case core::SampleFormat::Int16: {
                    const auto sample = static_cast<std::int16_t>(
                        std::lrint(value >= 1.0F ? 32767.0F : value * 32768.0F));
                    writeLittle(sample, destination, offset);
                    break;
                }
                case core::SampleFormat::Int24: {
                    floatToInt24(value, std::span<std::byte, 3>{destination.data() + offset, 3});
                    break;
                }
                case core::SampleFormat::Int32: {
                    const auto sample = static_cast<std::int32_t>(
                        std::llrint(value >= 1.0F ? 2147483647.0 : static_cast<double>(value) *
                                                                     2147483648.0));
                    writeLittle(sample, destination, offset);
                    break;
                }
                case core::SampleFormat::Float32:
                    writeLittle(value, destination, offset);
                    break;
            }
        }
    }

    return core::success();
}

float int24ToFloat(std::byte b0, std::byte b1, std::byte b2) noexcept {
    const std::int32_t raw = static_cast<std::int32_t>(std::to_integer<std::uint8_t>(b0)) |
                             (static_cast<std::int32_t>(std::to_integer<std::uint8_t>(b1)) << 8) |
                             (static_cast<std::int32_t>(std::to_integer<std::uint8_t>(b2)) << 16);
    const std::int32_t sample = (raw & 0x00800000) != 0 ? (raw | ~0x00FFFFFF) : raw;
    return static_cast<float>(sample) / kInt24Scale;
}

void floatToInt24(float value, std::span<std::byte, 3> destination) noexcept {
    const float clamped = clampUnit(value);
    const auto sample = static_cast<std::int32_t>(
        std::lrint(clamped >= 1.0F ? 8388607.0F : clamped * 8388608.0F));
    destination[0] = static_cast<std::byte>(sample & 0xFF);
    destination[1] = static_cast<std::byte>((sample >> 8) & 0xFF);
    destination[2] = static_cast<std::byte>((sample >> 16) & 0xFF);
}

}  // namespace aw::audio::convert
