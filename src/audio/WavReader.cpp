#include "aw/audio/WavReader.hpp"

#include "aw/audio/SampleConverter.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace aw::audio {
namespace {

template <typename T>
T little(std::span<const std::byte> bytes, std::size_t offset = 0) noexcept {
    std::array<std::byte, sizeof(T)> raw{};
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), sizeof(T), raw.begin());
    return std::bit_cast<T>(raw);
}

core::Status readExact(hal::FileHandle& file, std::span<std::byte> destination) {
    std::size_t offset = 0;
    while (offset < destination.size()) {
        auto read = file.read(destination.subspan(offset));
        if (!read) {
            return read.error();
        }
        if (read.value() == 0) {
            return core::makeError(core::ErrorCode::InvalidFormat, "unexpected end of file");
        }
        offset += read.value();
    }
    return core::success();
}

core::Status skipBytes(hal::FileHandle& file, std::uint64_t count) {
    const auto result = file.seek(static_cast<std::int64_t>(count), hal::SeekOrigin::Current);
    return result ? core::success() : core::Status{result.error()};
}

std::string trimInfoString(std::vector<std::byte> bytes) {
    while (!bytes.empty() &&
           (bytes.back() == std::byte{0} || bytes.back() == std::byte{'\r'} ||
            bytes.back() == std::byte{'\n'} || bytes.back() == std::byte{' '})) {
        bytes.pop_back();
    }
    std::string out;
    out.reserve(bytes.size());
    for (const std::byte byte : bytes) {
        out.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
    }
    return out;
}

void parseInfoList(std::span<const std::byte> bytes, WavMetadata& metadata) {
    if (bytes.size() < 4) {
        return;
    }
    if (std::string_view{reinterpret_cast<const char*>(bytes.data()), 4} != "INFO") {
        return;
    }

    std::size_t offset = 4;
    while (offset + 8 <= bytes.size()) {
        const std::string id{reinterpret_cast<const char*>(bytes.data() + offset), 4};
        const auto size = little<std::uint32_t>(bytes, offset + 4);
        offset += 8;
        if (offset + size > bytes.size()) {
            break;
        }

        std::vector<std::byte> value(size);
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), size, value.begin());
        const std::string text = trimInfoString(std::move(value));

        if (id == "INAM") {
            metadata.title = text;
        } else if (id == "IART") {
            metadata.artist = text;
        } else if (id == "ICMT") {
            metadata.comment = text;
        }

        offset += size;
        if ((size & 1U) != 0U && offset < bytes.size()) {
            ++offset;
        }
    }
}

core::Result<WavMetadata> parseHeader(hal::FileHandle& file) {
    const auto fileSizeResult = file.size();
    if (!fileSizeResult) {
        return fileSizeResult.error();
    }
    const std::uint64_t fileSize = fileSizeResult.value();
    if (fileSize < 12) {
        return core::makeError(core::ErrorCode::InvalidFormat, "truncated RIFF header");
    }

    std::array<std::byte, 12> riffHeader{};
    if (auto status = readExact(file, riffHeader); !status) {
        return status.error();
    }

    if (std::string_view{reinterpret_cast<const char*>(riffHeader.data()), 4} != "RIFF" ||
        std::string_view{reinterpret_cast<const char*>(riffHeader.data() + 8), 4} != "WAVE") {
        return core::makeError(core::ErrorCode::InvalidFormat, "not a RIFF/WAVE file");
    }

    const std::uint64_t riffEnd = 8U + little<std::uint32_t>(riffHeader, 4);
    if (riffEnd < 12 || riffEnd > fileSize) {
        return core::makeError(core::ErrorCode::InvalidFormat, "RIFF size exceeds file bounds");
    }

    WavMetadata metadata;
    bool haveFormat = false;
    bool haveData = false;

    while (true) {
        const auto chunkPosition = file.tell();
        if (!chunkPosition) {
            return chunkPosition.error();
        }
        if (chunkPosition.value() == riffEnd) {
            break;
        }
        if (chunkPosition.value() > riffEnd || riffEnd - chunkPosition.value() < 8) {
            return core::makeError(core::ErrorCode::InvalidFormat, "truncated chunk header");
        }

        std::array<std::byte, 8> chunkHeader{};
        if (auto status = readExact(file, chunkHeader); !status) {
            return status.error();
        }

        const std::string id{reinterpret_cast<const char*>(chunkHeader.data()), 4};
        const std::uint32_t size = little<std::uint32_t>(chunkHeader, 4);
        const std::uint64_t paddedSize = static_cast<std::uint64_t>(size) + (size & 1U);
        if (paddedSize > riffEnd - chunkPosition.value() - 8U) {
            return core::makeError(core::ErrorCode::InvalidFormat, "chunk exceeds RIFF bounds");
        }

        if (id == "fmt ") {
            if (size < 16) {
                return core::makeError(core::ErrorCode::InvalidFormat, "short fmt chunk");
            }

            std::vector<std::byte> fmt(size);
            if (auto status = readExact(file, fmt); !status) {
                return status.error();
            }

            const std::uint16_t audioFormat = little<std::uint16_t>(fmt, 0);
            const std::uint16_t channels = little<std::uint16_t>(fmt, 2);
            const std::uint32_t sampleRate = little<std::uint32_t>(fmt, 4);
            const std::uint16_t blockAlign = little<std::uint16_t>(fmt, 12);
            const std::uint16_t bitsPerSample = little<std::uint16_t>(fmt, 14);

            if (channels == 0 || channels > 12) {
                return core::makeError(core::ErrorCode::UnsupportedFormat,
                                       "WAV must contain between 1 and 12 channels");
            }
            if (sampleRate == 0) {
                return core::makeError(core::ErrorCode::InvalidFormat, "zero sample rate");
            }

            if (audioFormat == 1) {
                switch (bitsPerSample) {
                    case 16: metadata.format.sampleFormat = core::SampleFormat::Int16; break;
                    case 24: metadata.format.sampleFormat = core::SampleFormat::Int24; break;
                    case 32: metadata.format.sampleFormat = core::SampleFormat::Int32; break;
                    default:
                        return core::makeError(core::ErrorCode::UnsupportedFormat,
                                               "unsupported PCM bit depth");
                }
            } else if (audioFormat == 3 && bitsPerSample == 32) {
                metadata.format.sampleFormat = core::SampleFormat::Float32;
            } else {
                return core::makeError(core::ErrorCode::UnsupportedFormat,
                                       "unsupported WAV encoding");
            }

            metadata.format.channels = static_cast<core::ChannelCount>(channels);
            metadata.format.sampleRate = sampleRate;
            if (blockAlign != metadata.format.bytesPerFrame()) {
                return core::makeError(core::ErrorCode::InvalidFormat, "bad block align");
            }
            haveFormat = true;
        } else if (id == "data") {
            auto position = file.tell();
            if (!position) {
                return position.error();
            }
            metadata.dataOffset = position.value();
            metadata.dataBytes = size;
            haveData = true;

            if (auto status = skipBytes(file, size); !status) {
                return status.error();
            }
        } else if (id == "LIST") {
            constexpr std::uint32_t kMaxInfoBytes = 1024U * 1024U;
            if (size > kMaxInfoBytes) {
                if (auto status = skipBytes(file, size); !status) {
                    return status.error();
                }
            } else {
                std::vector<std::byte> listData(size);
                if (auto status = readExact(file, listData); !status) {
                    return status.error();
                }
                parseInfoList(listData, metadata);
            }
        } else {
            if (auto status = skipBytes(file, size); !status) {
                return status.error();
            }
        }

        if ((size & 1U) != 0U) {
            if (auto status = skipBytes(file, 1); !status) {
                return status.error();
            }
        }
    }

    if (!haveFormat || !haveData) {
        return core::makeError(core::ErrorCode::InvalidFormat, "missing fmt or data chunk");
    }

    if (metadata.format.bytesPerFrame() == 0 || metadata.dataBytes % metadata.format.bytesPerFrame() != 0) {
        return core::makeError(core::ErrorCode::InvalidFormat,
                               "data chunk is not a whole number of frames");
    }

    metadata.frameCount =
        static_cast<core::FrameCount>(metadata.dataBytes / metadata.format.bytesPerFrame());
    metadata.isNativeFormat =
        metadata.format.sampleRate == core::kNativeFormat.sampleRate &&
        metadata.format.sampleFormat == core::kNativeFormat.sampleFormat &&
        metadata.format.channels <= 12;
    return metadata;
}

}  // namespace

/// Reader state.  Lives behind a pimpl so the chunk table, the read cursor and
/// the conversion scratch buffer do not appear in a header every module includes.
struct WavReader::Impl {
    std::unique_ptr<hal::FileHandle> file;
    WavMetadata metadata;
    core::FrameIndex position{0};
    std::vector<std::byte> scratch;
};

WavReader::WavReader() = default;
WavReader::~WavReader() = default;
WavReader::WavReader(WavReader&&) noexcept = default;
WavReader& WavReader::operator=(WavReader&&) noexcept = default;

core::Status WavReader::open(hal::StorageInterface& storage, const core::Path& path) {
    close();

    auto file = storage.open(path, hal::OpenMode::Read);
    if (!file) {
        return file.error();
    }

    auto metadata = parseHeader(*file.value());
    if (!metadata) {
        return metadata.error();
    }

    auto seek = file.value()->seek(static_cast<std::int64_t>(metadata.value().dataOffset),
                                   hal::SeekOrigin::Begin);
    if (!seek) {
        return seek.error();
    }

    impl_ = std::make_unique<Impl>();
    impl_->file = std::move(file).value();
    impl_->metadata = std::move(metadata).value();
    impl_->position = 0;
    return core::success();
}

void WavReader::close() noexcept { impl_.reset(); }

bool WavReader::isOpen() const noexcept { return impl_ != nullptr && impl_->file != nullptr; }

const WavMetadata& WavReader::metadata() const noexcept {
    static const WavMetadata kEmpty{};
    return impl_ ? impl_->metadata : kEmpty;
}

core::Result<core::FrameCount> WavReader::read(AudioBuffer& destination) {
    if (!isOpen()) {
        return core::makeError(core::ErrorCode::InvalidState, "reader not open");
    }
    if (destination.channelCount() != impl_->metadata.format.channels) {
        return core::makeError(core::ErrorCode::InvalidArgument,
                               "destination channel count does not match WAV");
    }

    const core::FrameCount remaining = impl_->metadata.frameCount - impl_->position;
    const core::FrameCount targetFrames =
        std::max<core::FrameCount>(0, std::min(destination.capacityFrames(), remaining));
    destination.setFrameCount(targetFrames);
    if (targetFrames == 0) {
        return core::FrameCount{0};
    }

    const std::size_t bytesRequested =
        static_cast<std::size_t>(targetFrames) * impl_->metadata.format.bytesPerFrame();
    impl_->scratch.resize(bytesRequested);

    std::size_t bytesRead = 0;
    while (bytesRead < bytesRequested) {
        auto read = impl_->file->read(std::span<std::byte>{impl_->scratch}.subspan(bytesRead));
        if (!read) {
            return read.error();
        }
        if (read.value() == 0) {
            break;
        }
        bytesRead += read.value();
    }

    if (bytesRead % impl_->metadata.format.bytesPerFrame() != 0) {
        return core::makeError(core::ErrorCode::Corrupt, "partial frame at end of WAV read");
    }

    const core::FrameCount framesRead =
        static_cast<core::FrameCount>(bytesRead / impl_->metadata.format.bytesPerFrame());
    destination.setFrameCount(framesRead);

    std::vector<std::span<float>> channels;
    channels.reserve(destination.channelCount());
    for (core::ChannelCount channel = 0; channel < destination.channelCount(); ++channel) {
        channels.push_back(destination.channel(channel));
    }

    if (auto status = convert::toFloat(std::span<const std::byte>{impl_->scratch}.first(bytesRead),
                                       impl_->metadata.format, channels, framesRead);
        !status) {
        return status.error();
    }

    impl_->position += framesRead;
    return framesRead;
}

core::Status WavReader::seek(core::FrameIndex frame) {
    if (!isOpen()) {
        return core::makeError(core::ErrorCode::InvalidState, "reader not open");
    }

    const core::FrameIndex clamped = std::clamp<core::FrameIndex>(frame, 0, impl_->metadata.frameCount);
    const std::uint64_t byteOffset =
        impl_->metadata.dataOffset +
        static_cast<std::uint64_t>(clamped) * impl_->metadata.format.bytesPerFrame();
    auto seek = impl_->file->seek(static_cast<std::int64_t>(byteOffset), hal::SeekOrigin::Begin);
    if (!seek) {
        return seek.error();
    }

    impl_->position = clamped;
    return core::success();
}

core::FrameIndex WavReader::position() const noexcept { return impl_ ? impl_->position : 0; }

core::Result<WavMetadata> WavReader::readMetadata(hal::StorageInterface& storage,
                                                  const core::Path& path) {
    auto file = storage.open(path, hal::OpenMode::Read);
    if (!file) {
        return file.error();
    }
    return parseHeader(*file.value());
}

}  // namespace aw::audio
