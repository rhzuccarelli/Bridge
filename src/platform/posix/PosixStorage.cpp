#include "aw/platform/posix/PosixStorage.hpp"

#include <cstdio>
#include <filesystem>
#include <system_error>

namespace aw::platform::posix {
namespace {

/// Translates errno-family codes into the project's vocabulary.
core::ErrorCode translate(const std::error_code& ec) {
    if (ec == std::errc::no_such_file_or_directory) return core::ErrorCode::NotFound;
    if (ec == std::errc::file_exists) return core::ErrorCode::AlreadyExists;
    if (ec == std::errc::permission_denied) return core::ErrorCode::PermissionDenied;
    if (ec == std::errc::no_space_on_device) return core::ErrorCode::OutOfSpace;
    return core::ErrorCode::IoError;
}

core::Error fromErrorCode(const std::error_code& ec, const core::Path& path) {
    return core::makeError(translate(ec), path.string() + ": " + ec.message());
}

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

const char* modeString(hal::OpenMode mode) noexcept {
    switch (mode) {
        case hal::OpenMode::Read: return "rb";
        case hal::OpenMode::Write: return "wb";
        case hal::OpenMode::ReadWrite: return "r+b";
        case hal::OpenMode::Append: return "ab";
    }
    return "rb";
}

}  // namespace

PosixStorage::PosixStorage(core::Path projectsRoot) : projectsRoot_{std::move(projectsRoot)} {}

core::Result<std::unique_ptr<hal::FileHandle>> PosixStorage::open(const core::Path& path,
                                                                  hal::OpenMode mode) {
    std::FILE* file = std::fopen(path.c_str(), modeString(mode));
    if (file == nullptr && mode == hal::OpenMode::ReadWrite) {
        file = std::fopen(path.c_str(), "w+b");  // ReadWrite creates when missing.
    }
    if (file == nullptr) {
        return core::makeError(core::ErrorCode::NotFound, path.string());
    }
    return std::unique_ptr<hal::FileHandle>{std::make_unique<StdioFile>(file)};
}

bool PosixStorage::exists(const core::Path& path) const {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

bool PosixStorage::isDirectory(const core::Path& path) const {
    std::error_code ec;
    return std::filesystem::is_directory(path, ec);
}

core::Status PosixStorage::createDirectories(const core::Path& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec && !std::filesystem::is_directory(path)) {
        return fromErrorCode(ec, path);
    }
    return core::success();
}

core::Status PosixStorage::remove(const core::Path& path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    if (ec) {
        return fromErrorCode(ec, path);
    }
    return core::success();
}

core::Status PosixStorage::rename(const core::Path& from, const core::Path& to) {
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    if (ec) {
        return fromErrorCode(ec, from);
    }
    return core::success();
}

core::Result<std::vector<hal::DirectoryEntry>> PosixStorage::list(
    const core::Path& directory) const {
    std::error_code ec;
    std::vector<hal::DirectoryEntry> entries;

    for (const auto& entry : std::filesystem::directory_iterator{directory, ec}) {
        hal::DirectoryEntry item;
        item.name = entry.path().filename().string();
        item.isDirectory = entry.is_directory(ec);
        item.size = item.isDirectory ? 0 : entry.file_size(ec);
        entries.push_back(std::move(item));
    }

    if (ec) {
        return fromErrorCode(ec, directory);
    }
    return entries;
}

core::Result<std::uint64_t> PosixStorage::freeSpaceBytes(const core::Path& path) const {
    std::error_code ec;
    const std::filesystem::space_info info = std::filesystem::space(path, ec);
    if (ec) {
        return fromErrorCode(ec, path);
    }
    return static_cast<std::uint64_t>(info.available);
}

}  // namespace aw::platform::posix
