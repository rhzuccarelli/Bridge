#include "aw/audio/WavReader.hpp"

// Phase 2 implements RIFF chunk parsing and streaming reads.

namespace aw::audio {

/// Reader state.  Lives behind a pimpl so the chunk table, the read cursor and
/// the conversion scratch buffer do not appear in a header every module includes.
struct WavReader::Impl {
    std::unique_ptr<hal::FileHandle> file;
    WavMetadata metadata;
    core::FrameIndex position{0};
};

WavReader::WavReader() = default;
WavReader::~WavReader() = default;
WavReader::WavReader(WavReader&&) noexcept = default;
WavReader& WavReader::operator=(WavReader&&) noexcept = default;

core::Status WavReader::open(hal::StorageInterface& /*storage*/, const core::Path& path) {
    return core::notImplemented("WavReader::open " + path.string() + " (Phase 2)");
}

void WavReader::close() noexcept { impl_.reset(); }

bool WavReader::isOpen() const noexcept { return impl_ != nullptr && impl_->file != nullptr; }

const WavMetadata& WavReader::metadata() const noexcept {
    static const WavMetadata kEmpty{};
    return impl_ ? impl_->metadata : kEmpty;
}

core::Result<core::FrameCount> WavReader::read(AudioBuffer& /*destination*/) {
    return core::notImplemented("WavReader::read (Phase 2)");
}

core::Status WavReader::seek(core::FrameIndex /*frame*/) {
    return core::notImplemented("WavReader::seek (Phase 2)");
}

core::FrameIndex WavReader::position() const noexcept { return impl_ ? impl_->position : 0; }

core::Result<WavMetadata> WavReader::readMetadata(hal::StorageInterface& /*storage*/,
                                                  const core::Path& path) {
    return core::notImplemented("WavReader::readMetadata " + path.string() + " (Phase 2)");
}

}  // namespace aw::audio
