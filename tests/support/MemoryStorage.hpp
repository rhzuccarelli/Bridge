#pragma once

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "aw/hal/StorageInterface.hpp"

namespace awtest {

/// An in-RAM `hal::StorageInterface`.
///
/// The reason the HAL exists, demonstrated: WAV parsing, project saving, atomic
/// writes and import can all be tested against this - including error paths
/// like "the card is full" and "the file is truncated mid-header" - with no
/// filesystem, no cleanup, and no chance of a test polluting the developer's
/// machine.
class MemoryStorage final : public aw::hal::StorageInterface {
public:
    using Path = aw::core::Path;

    explicit MemoryStorage(Path projectsRoot = "/projects")
        : projectsRoot_{std::move(projectsRoot)} {
        directories_.insert(projectsRoot_.string());
    }

    /// Test helper: pre-populate a file.
    void addFile(const Path& path, std::vector<std::byte> contents) {
        files_[path.string()] = std::move(contents);
        addParentDirectories(path);
    }

    [[nodiscard]] const std::vector<std::byte>* fileContents(const Path& path) const {
        const auto it = files_.find(path.string());
        return it == files_.end() ? nullptr : &it->second;
    }

    [[nodiscard]] std::size_t readOpenCount(const Path& path) const {
        const auto it = readOpenCounts_.find(path.string());
        return it == readOpenCounts_.end() ? 0U : it->second;
    }

    /// Test helper: make every subsequent write fail with OutOfSpace.
    void setFreeSpace(std::uint64_t bytes) { freeSpace_ = bytes; }

    aw::core::Result<std::unique_ptr<aw::hal::FileHandle>> open(
        const Path& path, aw::hal::OpenMode mode) override;

    [[nodiscard]] bool exists(const Path& path) const override {
        return files_.count(path.string()) != 0 || directories_.count(path.string()) != 0;
    }

    [[nodiscard]] bool isDirectory(const Path& path) const override {
        return directories_.count(path.string()) != 0;
    }

    aw::core::Status createDirectories(const Path& path) override {
        addParentDirectories(path / "x");
        directories_.insert(path.string());
        return aw::core::success();
    }

    aw::core::Status remove(const Path& path) override {
        const bool removed = files_.erase(path.string()) > 0 ||
                             directories_.erase(path.string()) > 0;
        return removed ? aw::core::success()
                       : aw::core::makeError(aw::core::ErrorCode::NotFound, path.string());
    }

    aw::core::Status rename(const Path& from, const Path& to) override {
        const auto it = files_.find(from.string());
        if (it == files_.end()) {
            return aw::core::makeError(aw::core::ErrorCode::NotFound, from.string());
        }
        files_[to.string()] = std::move(it->second);
        files_.erase(it);
        return aw::core::success();
    }

    aw::core::Result<std::vector<aw::hal::DirectoryEntry>> list(
        const Path& directory) const override {
        std::vector<aw::hal::DirectoryEntry> entries;
        const std::string prefix = directory.string() + "/";
        std::set<std::string> seen;

        for (const auto& path : directories_) {
            if (path == directory.string()) {
                continue;
            }
            if (path.rfind(prefix, 0) == 0 &&
                path.find('/', prefix.size()) == std::string::npos &&
                seen.insert(path.substr(prefix.size())).second) {
                aw::hal::DirectoryEntry entry;
                entry.name = path.substr(prefix.size());
                entry.isDirectory = true;
                entries.push_back(std::move(entry));
            }
        }

        for (const auto& [path, contents] : files_) {
            if (path.rfind(prefix, 0) == 0 &&
                path.find('/', prefix.size()) == std::string::npos &&
                seen.insert(path.substr(prefix.size())).second) {
                aw::hal::DirectoryEntry entry;
                entry.name = path.substr(prefix.size());
                entry.size = contents.size();
                entries.push_back(std::move(entry));
            }
        }
        return entries;
    }

    aw::core::Result<std::uint64_t> freeSpaceBytes(const Path& /*path*/) const override {
        return freeSpace_;
    }

    [[nodiscard]] Path projectsRoot() const override { return projectsRoot_; }

private:
    void addParentDirectories(const Path& path) {
        for (Path parent = path.parent_path(); !parent.empty() && parent != parent.root_path();
             parent = parent.parent_path()) {
            directories_.insert(parent.string());
        }
    }

    Path projectsRoot_;
    std::map<std::string, std::vector<std::byte>> files_;
    std::map<std::string, std::size_t> readOpenCounts_;
    std::set<std::string> directories_;
    std::uint64_t freeSpace_{1ULL << 30};
};

}  // namespace awtest
