#pragma once

#include <memory>

#include "aw/core/Result.hpp"
#include "aw/hal/StorageInterface.hpp"

namespace aw::platform::esp32p4 {

/// SD card wiring and bus settings for SDMMC.
struct SdCardConfig {
    int clk{43};
    int cmd{44};
    int d0{39};
    int d1{40};
    int d2{41};
    int d3{42};
    /// On-chip LDO channel that powers the card's I/O, -1 for none. The P4
    /// reference design (and the P4-Pico) feed the slot from LDO VO4.
    int ldoChannel{4};
    /// Bus clock. 40 MHz is SD high speed; a card that cannot run it is
    /// mounted again at 20 MHz.
    int maxFrequencyKhz{40000};
    const char* mountPoint{"/sdcard"};
};

/// `hal::StorageInterface` on a FAT32/exFAT SD card through FatFS and the
/// ESP-IDF VFS: stdio for file contents, POSIX calls for directories.
///
/// FatFS's rename refuses to replace an existing file, and `AtomicFileWriter`
/// renames a temp file over `metadata.json`. `rename()` therefore replaces in
/// three steps:
///   1. `to` -> `to.old`   (the previous good version, kept aside),
///   2. `from` -> `to`,
///   3. delete `to.old`.
/// Power loss between 1 and 2 leaves only `to.old`: `open`, `exists` and
/// `isDirectory` restore it first, so the project reopens at its previous save
/// instead of not at all. Power loss between 2 and 3 leaves a stale `to.old`,
/// which the same recovery deletes. That is not atomic, but it never loses the
/// last good version.
///
/// Files are POSIX descriptors (`FatFsFile`), not stdio. The SD driver only
/// moves data by DMA in multi-sector commands when the buffer's address and
/// size are cache-aligned; anything else it writes one 512-byte sector per
/// command through a bounce buffer, about 11 times slower (0.48 against
/// 5.3 MiB/s on the bench). Import data arrives in PSRAM, shifted by the MTP
/// container header, so nearly every write took that path. Reads and writes
/// of 4 KiB or more therefore go through one shared, cache-aligned buffer in
/// internal DMA RAM, with the data placed so that the whole sectors FatFS
/// hands to the driver start aligned.
///
/// Newlib's `long` file offsets limit seeking to 2 GiB per file on this target.
class FatFsStorage final : public hal::StorageInterface {
public:
    /// Mounts the card (never formats it). Fails with DeviceNotConnected when
    /// there is no card or it is unreadable.
    [[nodiscard]] static core::Result<std::unique_ptr<FatFsStorage>> mount(
        const SdCardConfig& config, core::Path projectsRoot);
    ~FatFsStorage() override;

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
    FatFsStorage(const char* mountPoint, void* card, core::Path projectsRoot);

    /// Finishes an interrupted `rename()` that targeted `path`.
    void recover(const core::Path& path) const;

    const char* mountPoint_;
    void* card_;  ///< sdmmc_card_t*, kept out of this header.
    core::Path projectsRoot_;
};

}  // namespace aw::platform::esp32p4
