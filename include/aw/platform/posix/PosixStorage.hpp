#pragma once

#include "aw/hal/StorageInterface.hpp"

namespace aw::platform::posix {

/// `hal::StorageInterface` on top of `std::filesystem` and `std::fstream`.
///
/// Shared by the Raspberry Pi build and the simulator, because on both the card
/// is just a directory.  This is the one place in the codebase where
/// `std::filesystem` free functions are called; everything else goes through the
/// interface, which is what makes the ESP32's FatFS backend a drop-in.
///
/// `std::filesystem` throws.  Every call here is wrapped so the exception never
/// escapes as anything but a `core::Error`.
class PosixStorage final : public hal::StorageInterface {
public:
    explicit PosixStorage(core::Path projectsRoot);

    [[nodiscard]] core::Result<std::unique_ptr<hal::FileHandle>> open(
        const core::Path& path, hal::OpenMode mode) override;

    [[nodiscard]] bool exists(const core::Path& path) const override;
    [[nodiscard]] bool isDirectory(const core::Path& path) const override;

    [[nodiscard]] core::Status createDirectories(const core::Path& path) override;
    [[nodiscard]] core::Status remove(const core::Path& path) override;
    [[nodiscard]] core::Status rename(const core::Path& from, const core::Path& to) override;

    [[nodiscard]] core::Result<std::vector<hal::DirectoryEntry>> list(
        const core::Path& directory) const override;

    [[nodiscard]] core::Result<std::uint64_t> freeSpaceBytes(
        const core::Path& path) const override;

    [[nodiscard]] core::Path projectsRoot() const override { return projectsRoot_; }

private:
    core::Path projectsRoot_;
};

}  // namespace aw::platform::posix
