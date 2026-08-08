#include "aw/audio/WavWriter.hpp"

#include "aw/audio/SampleConverter.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <limits>
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

core::Status writeAll(hal::FileHandle& file, std::span<const std::byte> source) {
    std::size_t offset = 0;
    while (offset < source.size()) {
        auto written = file.write(source.subspan(offset));
        if (!written) {
            return written.error();
        }
        if (written.value() == 0) {
            return core::makeError(core::ErrorCode::IoError, "zero-length WAV write");
        }
        offset += written.value();
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
                         std::uint64_t dataChunkSizeOffset,
                         std::uint32_t riffBaseSize) {
    auto resume = file.tell();
    if (!resume) {
        return resume.error();
    }

    const std::uint64_t dataBytes =
        static_cast<std::uint64_t>(framesWritten) * format.bytesPerFrame();
    const std::uint32_t riffSize = static_cast<std::uint32_t>(riffBaseSize + dataBytes);
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
    std::uint32_t riffBaseSize{36};
    std::vector<std::byte> scratch;
    core::FrameCount nextCheckpoint{48000};
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
                               const core::AudioFormat& format,
                               const std::optional<AcidMetadata>& acid) {
    if (isOpen()) {
        if (auto status = close(); !status) {
            return status;
        }
    }
    if (format.sampleRate == 0 || format.channels == 0 || format.channels > 12 ||
        format.bytesPerFrame() == 0) {
        return core::makeError(core::ErrorCode::InvalidArgument, "invalid WAV format");
    }

    auto file = storage.open(path, hal::OpenMode::Write);
    if (!file) {
        return file.error();
    }

    std::vector<std::byte> header;
    header.reserve(acid ? 76 : 44);
    header.insert(header.end(),
                  {std::byte{'R'}, std::byte{'I'}, std::byte{'F'}, std::byte{'F'}});
    appendLittle<std::uint32_t>(header, acid ? 68U : 36U);
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
    if (acid) {
        header.insert(header.end(),
                      {std::byte{'a'}, std::byte{'c'}, std::byte{'i'}, std::byte{'d'}});
        appendLittle<std::uint32_t>(header, 24U);
        header.insert(header.end(), acid->bytes.begin(), acid->bytes.end());
    }
    header.insert(header.end(),
                  {std::byte{'d'}, std::byte{'a'}, std::byte{'t'}, std::byte{'a'}});
    appendLittle<std::uint32_t>(header, 0U);

    if (auto status = writeAll(*file.value(), header); !status) {
        return status;
    }
    if (auto status = file.value()->flush(); !status) {
        return status;
    }

    impl_ = std::make_unique<Impl>();
    impl_->file = std::move(file).value();
    impl_->format = format;
    impl_->framesWritten = 0;
    impl_->riffSizeOffset = 4;
    impl_->dataChunkSizeOffset = static_cast<std::uint64_t>(header.size() - 4U);
    impl_->riffBaseSize = static_cast<std::uint32_t>(impl_->dataChunkSizeOffset - 4U);
    impl_->nextCheckpoint = static_cast<core::FrameCount>(format.sampleRate);
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

    if (source.frameCount() < 0) {
        return core::makeError(core::ErrorCode::InvalidArgument, "negative WAV frame count");
    }
    const std::size_t byteCount =
        static_cast<std::size_t>(source.frameCount()) * impl_->format.bytesPerFrame();
    const std::uint64_t currentBytes =
        static_cast<std::uint64_t>(impl_->framesWritten) * impl_->format.bytesPerFrame();
    if (currentBytes + byteCount > std::numeric_limits<std::uint32_t>::max()) {
        return core::makeError(core::ErrorCode::InvalidArgument,
                               "WAV data exceeds the RIFF 32-bit size limit");
    }
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

    if (auto status = writeAll(*impl_->file, impl_->scratch); !status) {
        return status;
    }

    impl_->framesWritten += source.frameCount();
    if (impl_->framesWritten >= impl_->nextCheckpoint) {
        if (auto status = patchHeader(*impl_->file, impl_->format, impl_->framesWritten,
                                      impl_->riffSizeOffset, impl_->dataChunkSizeOffset,
                                      impl_->riffBaseSize);
            !status) {
            return status;
        }
        impl_->nextCheckpoint =
            impl_->framesWritten + static_cast<core::FrameCount>(impl_->format.sampleRate);
    }
    return core::success();
}

core::Status WavWriter::close() {
    if (!isOpen()) {
        return core::success();
    }

    core::Status status = patchHeader(*impl_->file, impl_->format, impl_->framesWritten,
                                      impl_->riffSizeOffset, impl_->dataChunkSizeOffset,
                                      impl_->riffBaseSize);
    impl_.reset();
    return status;
}

bool WavWriter::isOpen() const noexcept { return impl_ != nullptr && impl_->file != nullptr; }

core::FrameCount WavWriter::framesWritten() const noexcept {
    return impl_ ? impl_->framesWritten : 0;
}

}  // namespace aw::audio
