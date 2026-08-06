#pragma once

#include <memory>

#include "aw/audio/AudioBuffer.hpp"
#include "aw/core/AudioFormat.hpp"
#include "aw/core/Result.hpp"
#include "aw/core/Types.hpp"
#include "aw/hal/StorageInterface.hpp"

namespace aw::audio {

/// Streaming writer for RIFF/WAVE PCM files.
///
/// Phase 2 implements this.  Two rules it must honour:
///  - The RIFF and data chunk sizes are patched on `close()`, and the file is
///    also left *playable* if power is lost mid-write: sizes are rewritten
///    periodically, so a crashed recording loses its tail rather than all of it.
///  - It writes 24 bit / 48 kHz by default, matching the project format, so
///    exported files drop straight back onto the TP-7 without conversion.
///
/// Not thread safe.  One writer per recording or export.
class WavWriter {
public:
    /// Defined out of line for the same reason as `WavReader` - see there.
    WavWriter();
    ~WavWriter();

    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;
    WavWriter(WavWriter&&) noexcept;
    WavWriter& operator=(WavWriter&&) noexcept;

    /// Creates (or truncates) `path` and writes a provisional header.
    [[nodiscard]] core::Status create(hal::StorageInterface& storage,
                                      const core::Path& path,
                                      const core::AudioFormat& format);

    /// Appends `source.frameCount()` frames, converting from float.
    [[nodiscard]] core::Status write(const AudioBuffer& source);

    /// Patches the header with the final sizes and closes.  Must be called;
    /// the destructor does it too but cannot report failure.
    [[nodiscard]] core::Status close();

    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] core::FrameCount framesWritten() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aw::audio
