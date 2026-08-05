#include "aw/audio/WavWriter.hpp"

// Phase 2 implements header emission, streaming writes and size patching.

namespace aw::audio {

struct WavWriter::Impl {
    std::unique_ptr<hal::FileHandle> file;
    core::AudioFormat format{};
    core::FrameCount framesWritten{0};
    std::uint64_t dataChunkSizeOffset{0};
};

WavWriter::WavWriter() = default;
WavWriter::~WavWriter() = default;
WavWriter::WavWriter(WavWriter&&) noexcept = default;
WavWriter& WavWriter::operator=(WavWriter&&) noexcept = default;

core::Status WavWriter::create(hal::StorageInterface& /*storage*/,
                               const core::Path& path,
                               const core::AudioFormat& /*format*/) {
    return core::notImplemented("WavWriter::create " + path.string() + " (Phase 2)");
}

core::Status WavWriter::write(const AudioBuffer& /*source*/) {
    return core::notImplemented("WavWriter::write (Phase 2)");
}

core::Status WavWriter::close() { return core::notImplemented("WavWriter::close (Phase 2)"); }

bool WavWriter::isOpen() const noexcept { return impl_ != nullptr && impl_->file != nullptr; }

core::FrameCount WavWriter::framesWritten() const noexcept {
    return impl_ ? impl_->framesWritten : 0;
}

}  // namespace aw::audio
