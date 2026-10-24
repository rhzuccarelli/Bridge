#pragma once

#include <cstdio>
#include <span>

#include "aw/hal/StorageInterface.hpp"

namespace aw::platform {

/// `hal::FileHandle` over a C stdio stream.
///
/// `FILE*` rather than `std::fstream`: the stream API's error reporting is
/// awkward to map onto `Result`, and stdio gives direct control over buffering,
/// which matters when the same class is used for both 4 KiB metadata writes and
/// megabyte-scale audio streaming.
class StdioFile final : public hal::FileHandle {
public:
    explicit StdioFile(std::FILE* file) noexcept : file_{file} {}

    ~StdioFile() override {
        if (file_ != nullptr) {
            std::fclose(file_);
        }
    }

    core::Result<std::size_t> read(std::span<std::byte> destination) override {
        const std::size_t count = std::fread(destination.data(), 1, destination.size(), file_);
        if (count < destination.size() && std::ferror(file_) != 0) {
            return core::makeError(core::ErrorCode::IoError, "fread failed");
        }
        return count;
    }

    core::Result<std::size_t> write(std::span<const std::byte> source) override {
        const std::size_t count = std::fwrite(source.data(), 1, source.size(), file_);
        if (count < source.size()) {
            return core::makeError(core::ErrorCode::IoError, "fwrite failed");
        }
        return count;
    }

    core::Result<std::uint64_t> seek(std::int64_t offset, hal::SeekOrigin origin) override {
        const int whence = origin == hal::SeekOrigin::Begin     ? SEEK_SET
                           : origin == hal::SeekOrigin::Current ? SEEK_CUR
                                                                : SEEK_END;
        if (std::fseek(file_, static_cast<long>(offset), whence) != 0) {
            return core::makeError(core::ErrorCode::IoError, "fseek failed");
        }
        return tell();
    }

    core::Result<std::uint64_t> tell() const override {
        const long position = std::ftell(file_);
        if (position < 0) {
            return core::makeError(core::ErrorCode::IoError, "ftell failed");
        }
        return static_cast<std::uint64_t>(position);
    }

    core::Result<std::uint64_t> size() const override {
        const long current = std::ftell(file_);
        if (current < 0 || std::fseek(file_, 0, SEEK_END) != 0) {
            return core::makeError(core::ErrorCode::IoError, "fseek failed");
        }
        const long end = std::ftell(file_);
        std::fseek(file_, current, SEEK_SET);
        if (end < 0) {
            return core::makeError(core::ErrorCode::IoError, "ftell failed");
        }
        return static_cast<std::uint64_t>(end);
    }

    core::Status flush() override {
        if (std::fflush(file_) != 0) {
            return core::makeError(core::ErrorCode::IoError, "fflush failed");
        }
        return core::success();
    }

private:
    std::FILE* file_{nullptr};
};

/// fopen mode for an `OpenMode`. ReadWrite needs a "w+b" retry when the file
/// is missing: "r+b" does not create.
inline const char* stdioMode(hal::OpenMode mode) noexcept {
    switch (mode) {
        case hal::OpenMode::Read: return "rb";
        case hal::OpenMode::Write: return "wb";
        case hal::OpenMode::ReadWrite: return "r+b";
        case hal::OpenMode::Append: return "ab";
    }
    return "rb";
}


}  // namespace aw::platform
