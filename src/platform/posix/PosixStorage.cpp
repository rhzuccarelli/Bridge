#include "aw/platform/posix/PosixStorage.hpp"

#include "aw/platform/StdioFile.hpp"

#include <cstdio>
#include <filesystem>
#include <system_error>

namespace aw::platform::posix {

using platform::StdioFile;
using platform::stdioMode;
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

}  // namespace

PosixStorage::PosixStorage(core::Path projectsRoot) : projectsRoot_{std::move(projectsRoot)} {}

core::Result<std::unique_ptr<hal::FileHandle>> PosixStorage::open(const core::Path& path,
                                                                  hal::OpenMode mode) {
    std::FILE* file = std::fopen(path.c_str(), stdioMode(mode));
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
