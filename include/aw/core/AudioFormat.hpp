#pragma once

#include <cstddef>
#include <cstdint>

#include "aw/core/Types.hpp"

namespace aw::core {

/// Sample encodings the device can read or write on *storage*.
///
/// Internally the mixer always works in `float`.  These values describe files
/// and the hardware DAC only, which is why 24 bit packed (the TP-7's format)
/// is a first class member rather than something the engine has to think about.
enum class SampleFormat : std::uint8_t {
    Int16,    ///< 2 bytes, little endian.
    Int24,    ///< 3 bytes packed, little endian.  The project's primary format.
    Int32,    ///< 4 bytes, little endian.
    Float32,  ///< IEEE 754, little endian.  Accepted on import, not produced.
};

[[nodiscard]] constexpr std::size_t bytesPerSample(SampleFormat format) noexcept {
    switch (format) {
        case SampleFormat::Int16: return 2;
        case SampleFormat::Int24: return 3;
        case SampleFormat::Int32: return 4;
        case SampleFormat::Float32: return 4;
    }
    return 0;
}

/// Description of an interleaved PCM stream.
struct AudioFormat {
    SampleRate sampleRate{48000};
    ChannelCount channels{2};
    SampleFormat sampleFormat{SampleFormat::Int24};

    [[nodiscard]] constexpr std::size_t bytesPerFrame() const noexcept {
        return bytesPerSample(sampleFormat) * channels;
    }
};

[[nodiscard]] constexpr bool operator==(const AudioFormat& a, const AudioFormat& b) noexcept {
    return a.sampleRate == b.sampleRate && a.channels == b.channels &&
           a.sampleFormat == b.sampleFormat;
}

/// The one format the whole device is built around.
///
/// A single project sample rate is a deliberate product decision, not a
/// limitation we are apologising for: it removes resampling from the realtime
/// path entirely, which is what makes multitrack playback feasible on an
/// ESP32-P4.  Files that do not match are converted once, on import.
inline constexpr AudioFormat kNativeFormat{48000, 2, SampleFormat::Int24};

}  // namespace aw::core
