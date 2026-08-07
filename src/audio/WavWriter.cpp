#include "aw/audio/WavWriter.hpp"

#include "aw/audio/SampleConverter.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <vector>

namespace aw::audio {
namespace {

template <typename T>
void appendLittle(std::vector<std::byte>& bytes, T value) {
    const auto raw = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
    bytes.insert(bytes.end(), raw.begin(), raw.end());
}

template <typename T>
core::Status writeLittleAt(hal::FileHandle& file, std::uint64_t offset, T value) {
    auto seek = file.seek(static_cast<std::int64_t>(offset), hal::SeekOrigin::Begin);
    if (!seek) {
        return seek.error();
    }

    const auto raw = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
    auto written = file.write(raw);
    if (!written) {
        return written.error();
    }
    if (written.value() != raw.size()) {
        return core::makeError(core::ErrorCode::IoError, "short WAV header patch write");
    }
    return core::success();
}

std::uint16_t wavFormatCode(core::SampleFormat format) {
    return format == core::SampleFormat::Float32 ? 3U : 1U;
}

core::Status patchHeader(hal::FileHandle& file,
                         const core::AudioFormat& format,
                         core::FrameCount framesWritten,
                         std::uint64_t riffSizeOffset,
                         std::uint64_t dataChunkSizeOffset) {
    auto resume = file.tell();
    if (!resume) {
        return resume.error();
    }

    const std::uint64_t dataBytes =
        static_cast<std::uint64_t>(framesWritten) * format.bytesPerFrame();
    const std::uint32_t riffSize = static_cast<std::uint32_t>(36U + dataBytes);
    const std::uint32_t dataSize = static_cast<std::uint32_t>(dataBytes);

    if (auto status = writeLittleAt(file, riffSizeOffset, riffSize); !status) {
        return status;
    }
    if (auto status = writeLittleAt(file, dataChunkSizeOffset, dataSize); !status) {
        return status;
    }
    if (auto seek = file.seek(static_cast<std::int64_t>(resume.value()), hal::SeekOrigin::Begin);
        !seek) {
        return seek.error();
    }
    return file.flush();
}

}  // namespace

struct WavWriter::Impl {
    std::unique_ptr<hal::FileHandle> file;
    core::AudioFormat format{};
    core::FrameCount framesWritten{0};
    std::uint64_t riffSizeOffset{4};
    std::uint64_t dataChunkSizeOffset{0};
    std::vector<std::byte> scratch;
};

WavWriter::WavWriter() = default;
WavWriter::~WavWriter() {
    if (isOpen()) {
        (void)close();
    }
}
WavWriter::WavWriter(WavWriter&&) noexcept = default;
WavWriter& WavWriter::operator=(WavWriter&&) noexcept = default;

core::Status WavWriter::create(hal::StorageInterface& storage,
                               const core::Path& path,
                               const core::AudioFormat& format) {
    if (isOpen()) {
        if (auto status = close(); !status) {
            return status;
        }
    }
    if (format.sampleRate == 0 || format.channels == 0 || format.bytesPerFrame() == 0) {
        return core::makeError(core::ErrorCode::InvalidArgument, "invalid WAV format");
    }

    auto file = storage.open(path, hal::OpenMode::Write);
    if (!file) {
        return file.error();
    }

    std::vector<std::byte> header;
    header.reserve(44);
    header.insert(header.end(),
                  {std::byte{'R'}, std::byte{'I'}, std::byte{'F'}, std::byte{'F'}});
    appendLittle<std::uint32_t>(header, 36U);
    header.insert(header.end(),
                  {std::byte{'W'}, std::byte{'A'}, std::byte{'V'}, std::byte{'E'}});
    header.insert(header.end(),
                  {std::byte{'f'}, std::byte{'m'}, std::byte{'t'}, std::byte{' '}});
    appendLittle<std::uint32_t>(header, 16U);
    appendLittle<std::uint16_t>(header, wavFormatCode(format.sampleFormat));
    appendLittle<std::uint16_t>(header, format.channels);
    appendLittle<std::uint32_t>(header, format.sampleRate);
    appendLittle<std::uint32_t>(
        header, format.sampleRate * static_cast<std::uint32_t>(format.bytesPerFrame()));
    appendLittle<std::uint16_t>(header, static_cast<std::uint16_t>(format.bytesPerFrame()));
    appendLittle<std::uint16_t>(
        header,
        static_cast<std::uint16_t>(core::bytesPerSample(format.sampleFormat) * 8U));
    header.insert(header.end(),
                  {std::byte{'d'}, std::byte{'a'}, std::byte{'t'}, std::byte{'a'}});
    appendLittle<std::uint32_t>(header, 0U);

    auto written = file.value()->write(header);
    if (!written) {
        return written.error();
    }
    if (written.value() != header.size()) {
        return core::makeError(core::ErrorCode::IoError, "short WAV header write");
    }
    if (auto status = file.value()->flush(); !status) {
        return status;
    }

    impl_ = std::make_unique<Impl>();
    impl_->file = std::move(file).value();
    impl_->format = format;
    impl_->framesWritten = 0;
    impl_->riffSizeOffset = 4;
    impl_->dataChunkSizeOffset = 40;
    return core::success();
}

core::Status WavWriter::write(const AudioBuffer& source) {
    if (!isOpen()) {
        return core::makeError(core::ErrorCode::InvalidState, "writer not open");
    }
    if (source.channelCount() != impl_->format.channels) {
        return core::makeError(core::ErrorCode::InvalidArgument,
                               "source channel count does not match WAV format");
    }

    const std::size_t byteCount =
        static_cast<std::size_t>(source.frameCount()) * impl_->format.bytesPerFrame();
    impl_->scratch.resize(byteCount);

    std::vector<std::span<const float>> channels;
    channels.reserve(source.channelCount());
    for (core::ChannelCount channel = 0; channel < source.channelCount(); ++channel) {
        channels.push_back(source.channel(channel));
    }

    if (auto status = convert::fromFloat(channels, impl_->format, impl_->scratch, source.frameCount());
        !status) {
        return status;
    }

    auto written = impl_->file->write(impl_->scratch);
    if (!written) {
        return written.error();
    }
    if (written.value() != byteCount) {
        return core::makeError(core::ErrorCode::IoError, "short WAV data write");
    }

    impl_->framesWritten += source.frameCount();
    return patchHeader(*impl_->file, impl_->format, impl_->framesWritten, impl_->riffSizeOffset,
                       impl_->dataChunkSizeOffset);
}

core::Status WavWriter::close() {
    if (!isOpen()) {
        return core::success();
    }

    core::Status status = patchHeader(*impl_->file, impl_->format, impl_->framesWritten,
                                      impl_->riffSizeOffset, impl_->dataChunkSizeOffset);
    impl_.reset();
    return status;
}

bool WavWriter::isOpen() const noexcept { return impl_ != nullptr && impl_->file != nullptr; }

core::FrameCount WavWriter::framesWritten() const noexcept {
    return impl_ ? impl_->framesWritten : 0;
}

}  // namespace aw::audio
