#include "aw/audio/WavReader.hpp"

#include "aw/audio/SampleConverter.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
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
        } else if (id == "acid" && size == 24) {
            AcidMetadata acid;
            if (auto status = readExact(file, acid.bytes); !status) return status.error();
            metadata.acid = acid;
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
    /// Background read-ahead: a thread that reads the next chunk while the
    /// current one is used. It alone touches `file` while it exists.
    struct Prefetch {
        enum class State : std::uint8_t { Idle, Pending, Ready };
        std::mutex mutex;
        std::condition_variable changed;
        State state{State::Idle};
        bool stopping{false};
        core::FrameIndex start{0};
        core::FrameCount wanted{0};
        core::FrameCount frames{0};
        core::Status status = core::success();
        std::vector<std::byte> buffer;
        std::thread thread;
    };

    ~Impl() { stopPrefetch(); }

    void stopPrefetch() {
        if (!prefetch) {
            return;
        }
        {
            std::lock_guard lock{prefetch->mutex};
            prefetch->stopping = true;
        }
        prefetch->changed.notify_all();
        prefetch->thread.join();
        prefetch.reset();
    }

    /// Reads up to `wanted` frames from `start` into `buffer`.
    core::Result<core::FrameCount> fill(std::vector<std::byte>& buffer, core::FrameIndex start,
                                        core::FrameCount wanted) {
        const std::size_t bytesPerFrame = metadata.format.bytesPerFrame();
        if (filePosition != start) {
            auto seek = file->seek(static_cast<std::int64_t>(
                                       metadata.dataOffset +
                                       static_cast<std::uint64_t>(start) * bytesPerFrame),
                                   hal::SeekOrigin::Begin);
            if (!seek) {
                filePosition = -1;
                return seek.error();
            }
            filePosition = start;
        }
        const std::size_t bytesWanted = static_cast<std::size_t>(wanted) * bytesPerFrame;
        std::size_t bytesRead = 0;
        while (bytesRead < bytesWanted) {
            auto read =
                file->read(std::span<std::byte>{buffer}.subspan(bytesRead, bytesWanted - bytesRead));
            if (!read) {
                filePosition = -1;
                return read.error();
            }
            if (read.value() == 0) {
                break;
            }
            bytesRead += read.value();
        }
        if (bytesRead % bytesPerFrame != 0) {
            filePosition = -1;
            return core::makeError(core::ErrorCode::Corrupt, "partial frame at end of WAV read");
        }
        const auto frames = static_cast<core::FrameCount>(bytesRead / bytesPerFrame);
        filePosition += frames;
        return frames;
    }

    void runPrefetch() {
        Prefetch& shared = *prefetch;
        for (;;) {
            std::unique_lock lock{shared.mutex};
            shared.changed.wait(lock, [&] {
                return shared.stopping || shared.state == Prefetch::State::Pending;
            });
            if (shared.stopping) {
                return;
            }
            const core::FrameIndex start = shared.start;
            const core::FrameCount wanted = shared.wanted;
            lock.unlock();
            auto got = fill(shared.buffer, start, wanted);
            lock.lock();
            shared.frames = got ? got.value() : 0;
            shared.status = got ? core::success() : core::Status{got.error()};
            shared.state = Prefetch::State::Ready;
            shared.changed.notify_all();
        }
    }

    /// Asks the prefetch thread for the chunk at `start`. Holds `lock`.
    void request(std::unique_lock<std::mutex>& lock, core::FrameIndex start) {
        prefetch->changed.wait(lock, [&] { return prefetch->state != Prefetch::State::Pending; });
        prefetch->state = Prefetch::State::Pending;
        prefetch->start = start;
        prefetch->wanted = std::min(aheadCapacity, metadata.frameCount - start);
        prefetch->changed.notify_all();
    }

    /// Makes `ahead` hold the chunk that starts at `position`.
    core::Status loadChunk() {
        aheadStart = position;
        aheadFrames = 0;
        if (!prefetch) {
            auto got = fill(ahead, position, std::min(aheadCapacity, metadata.frameCount - position));
            if (!got) {
                return got.error();
            }
            aheadFrames = got.value();
            return core::success();
        }
        std::unique_lock lock{prefetch->mutex};
        if (prefetch->state == Prefetch::State::Idle || prefetch->start != position) {
            request(lock, position);
        }
        prefetch->changed.wait(lock, [&] { return prefetch->state == Prefetch::State::Ready; });
        prefetch->state = Prefetch::State::Idle;
        if (!prefetch->status) {
            return prefetch->status;
        }
        std::swap(ahead, prefetch->buffer);
        aheadFrames = prefetch->frames;
        // Start on the next chunk while this one is used.
        if (aheadFrames > 0 && position + aheadFrames < metadata.frameCount) {
            request(lock, position + aheadFrames);
        }
        return core::success();
    }

    std::unique_ptr<hal::FileHandle> file;
    WavMetadata metadata;
    core::FrameIndex position{0};
    std::vector<std::byte> scratch;
    // Read-ahead: `ahead` holds frames [aheadStart, aheadStart + aheadFrames),
    // and the next file read starts at `filePosition` (-1: unknown). Without
    // read-ahead the file is always at `position`.
    core::FrameCount aheadCapacity{0};
    std::vector<std::byte> ahead;
    core::FrameIndex aheadStart{0};
    core::FrameCount aheadFrames{0};
    core::FrameIndex filePosition{0};
    std::unique_ptr<Prefetch> prefetch;
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

void WavReader::setReadAhead(std::size_t bytes, bool background) {
    if (!isOpen()) {
        return;
    }
    impl_->stopPrefetch();
    if (impl_->aheadCapacity == 0) {
        impl_->filePosition = impl_->position;  // Plain reads leave the file there.
    }
    const std::size_t bytesPerFrame = impl_->metadata.format.bytesPerFrame();
    impl_->aheadCapacity =
        bytesPerFrame == 0 ? 0 : static_cast<core::FrameCount>(bytes / bytesPerFrame);
    impl_->ahead.resize(static_cast<std::size_t>(impl_->aheadCapacity) * bytesPerFrame);
    impl_->ahead.shrink_to_fit();
    impl_->aheadFrames = 0;
    if (impl_->aheadCapacity == 0) {
        (void)seek(impl_->position);  // Back to the plain reads' rule.
        return;
    }
    if (background) {
        impl_->prefetch = std::make_unique<Impl::Prefetch>();
        impl_->prefetch->buffer.resize(impl_->ahead.size());
        impl_->prefetch->thread = std::thread{[impl = impl_.get()] { impl->runPrefetch(); }};
    }
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
    if (impl_->aheadCapacity > 0) {
        return readAhead(destination, targetFrames);
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

core::Result<core::FrameCount> WavReader::readAhead(AudioBuffer& destination,
                                                    core::FrameCount targetFrames) {
    const core::AudioFormat& format = impl_->metadata.format;
    const std::size_t bytesPerFrame = format.bytesPerFrame();
    std::vector<std::span<float>> channels(destination.channelCount());
    core::FrameCount done = 0;
    while (done < targetFrames) {
        if (impl_->position < impl_->aheadStart ||
            impl_->position >= impl_->aheadStart + impl_->aheadFrames) {
            if (auto status = impl_->loadChunk(); !status) {
                return status.error();
            }
            if (impl_->aheadFrames == 0) {
                break;
            }
        }
        const core::FrameCount offset = impl_->position - impl_->aheadStart;
        const core::FrameCount frames = std::min(targetFrames - done, impl_->aheadFrames - offset);
        for (core::ChannelCount channel = 0; channel < destination.channelCount(); ++channel) {
            channels[channel] = destination.channel(channel).subspan(static_cast<std::size_t>(done),
                                                                     static_cast<std::size_t>(frames));
        }
        const auto bytes = std::span<const std::byte>{impl_->ahead}.subspan(
            static_cast<std::size_t>(offset) * bytesPerFrame,
            static_cast<std::size_t>(frames) * bytesPerFrame);
        if (auto status = convert::toFloat(bytes, format, channels, frames); !status) {
            return status.error();
        }
        impl_->position += frames;
        done += frames;
    }
    destination.setFrameCount(done);
    return done;
}

core::Status WavReader::seek(core::FrameIndex frame) {
    if (!isOpen()) {
        return core::makeError(core::ErrorCode::InvalidState, "reader not open");
    }
    impl_->aheadFrames = 0;

    const core::FrameIndex clamped = std::clamp<core::FrameIndex>(frame, 0, impl_->metadata.frameCount);
    if (impl_->aheadCapacity > 0) {
        impl_->position = clamped;  // The next chunk read seeks the file.
        return core::success();
    }
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
