#pragma once

#include <cstdint>
#include <string>

#include "aw/core/AudioFormat.hpp"
#include "aw/core/Types.hpp"

namespace aw::audio {

/// Everything known about a WAV file without reading its audio data.
///
/// Kept separate from `WavReader` so the import browser can show duration and
/// format for a hundred files by parsing a hundred headers, without opening a
/// hundred streams.
struct WavMetadata {
    core::AudioFormat format{};

    /// Total frames of audio in the data chunk.
    core::FrameCount frameCount{0};

    /// Byte offset of the first sample, and the data chunk's length.
    std::uint64_t dataOffset{0};
    std::uint64_t dataBytes{0};

    /// True when the file is RIFF/WAVE with PCM data this device can play
    /// directly.  False means it needs conversion on import (or rejection).
    bool isNativeFormat{false};

    /// Optional LIST/INFO metadata, when the source device writes it.
    /// The TP-7 names its recordings, which is what the browser shows.
    std::string title;
    std::string artist;
    std::string comment;

    [[nodiscard]] std::int64_t durationMillis() const noexcept {
        return core::framesToMillis(frameCount, format.sampleRate);
    }
};

}  // namespace aw::audio
