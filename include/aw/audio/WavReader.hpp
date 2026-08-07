#pragma once

#include <memory>

#include "aw/audio/AudioBuffer.hpp"
#include "aw/audio/WavMetadata.hpp"
#include "aw/core/Result.hpp"
#include "aw/core/Types.hpp"
#include "aw/hal/StorageInterface.hpp"

namespace aw::audio {

/// Streaming reader for RIFF/WAVE PCM files.
///
/// Phase 2 implements this.  Design commitments made here because they shape
/// every layer above:
///  - Never loads a whole file.  `read()` fills a caller-owned buffer.
///  - Takes a `hal::StorageInterface&`, so it can be tested against an in-memory
///    volume with hand built (and deliberately malformed) files.
///  - Parses the chunk list properly rather than assuming `fmt` then `data` at
///    fixed offsets - the TP-7 and most recorders emit extra chunks.
///
/// Not thread safe.  One reader per track, owned by the streaming thread.
class WavReader {
public:
    /// Defined out of line: with a pimpl member, an inline constructor would
    /// force `Impl` to be complete in every translation unit that includes this.
    WavReader();
    ~WavReader();

    WavReader(const WavReader&) = delete;
    WavReader& operator=(const WavReader&) = delete;
    WavReader(WavReader&&) noexcept;
    WavReader& operator=(WavReader&&) noexcept;

    /// Opens the file and parses its header.  Fails with InvalidFormat on a
    /// malformed container, UnsupportedFormat on something well formed we do
    /// not play (compressed, > 12 channels, unsupported bit depth).
    [[nodiscard]] core::Status open(hal::StorageInterface& storage, const core::Path& path);

    void close() noexcept;
    [[nodiscard]] bool isOpen() const noexcept;

    [[nodiscard]] const WavMetadata& metadata() const noexcept;

    /// Reads up to `destination.capacityFrames()` frames from the current
    /// position, converting to float.  Sets the buffer's frame count to what was
    /// actually read; 0 means end of file.
    ///
    /// Blocking: called from the streaming thread, never from the audio callback.
    [[nodiscard]] core::Result<core::FrameCount> read(AudioBuffer& destination);

    /// Positions the next read.  Clamped to [0, frameCount].
    [[nodiscard]] core::Status seek(core::FrameIndex frame);

    [[nodiscard]] core::FrameIndex position() const noexcept;

    /// Header-only parse, for browsers and import previews.
    [[nodiscard]] static core::Result<WavMetadata> readMetadata(hal::StorageInterface& storage,
                                                                const core::Path& path);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;  ///< pimpl: keeps chunk parsing state out of this header.
};

}  // namespace aw::audio
