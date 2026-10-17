#include "aw/platform/esp32p4/FatFsStorage.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "aw/core/Logger.hpp"
#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"

namespace aw::platform::esp32p4 {
namespace {

constexpr std::string_view kModule = "sd";

/// The shared transfer buffer (see the class comment in the header), and the
/// size from which a read or write goes through it. Below that, FatFS's own
/// per-file sector cache handles the bytes anyway.
constexpr std::size_t kBounceBytes = 64 * 1024;
constexpr std::size_t kBounceFrom = 4 * 1024;

struct Bounce {
    std::mutex mutex;
    std::uint8_t* data{nullptr};
    /// At least the P4's largest cache line (the driver's alignment test), and
    /// a divisor of the 512-byte sector.
    std::size_t alignment{128};

    Bounce() {
        data = static_cast<std::uint8_t*>(
            heap_caps_aligned_alloc(alignment, kBounceBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    }

    /// Where data for file offset `position` goes in the buffer: a whole
    /// sector after a partial first one then starts on an aligned address.
    [[nodiscard]] std::size_t padFor(std::uint64_t position) const {
        return static_cast<std::size_t>(position % alignment);
    }
};

Bounce& bounce() {
    static Bounce shared;
    return shared;
}

core::Error errnoError(std::string_view what) {
    return core::makeError(errno == ENOSPC ? core::ErrorCode::OutOfSpace : core::ErrorCode::IoError,
                           std::string{what} + ": " + std::strerror(errno));
}

/// `hal::FileHandle` over a FatFS file descriptor; see the header.
class FatFsFile final : public hal::FileHandle {
public:
    FatFsFile(int fd, bool append) noexcept : fd_{fd}, append_{append} {}
    ~FatFsFile() override { ::close(fd_); }

    core::Result<std::size_t> read(std::span<std::byte> destination) override {
        Bounce& shared = bounce();
        if (destination.size() < kBounceFrom || shared.data == nullptr) {
            return direct(::read(fd_, destination.data(), destination.size()), "read");
        }
        std::size_t done = 0;
        while (done < destination.size()) {
            const std::lock_guard lock{shared.mutex};
            const std::size_t pad = shared.padFor(position());
            const std::size_t wanted = std::min(destination.size() - done, kBounceBytes - shared.alignment);
            const ssize_t got = ::read(fd_, shared.data + pad, wanted);
            if (got < 0) return errnoError("read");
            std::memcpy(destination.data() + done, shared.data + pad, static_cast<std::size_t>(got));
            done += static_cast<std::size_t>(got);
            if (static_cast<std::size_t>(got) < wanted) break;  // End of file.
        }
        return done;
    }

    core::Result<std::size_t> write(std::span<const std::byte> source) override {
        Bounce& shared = bounce();
        if (source.size() < kBounceFrom || shared.data == nullptr) {
            auto written = direct(::write(fd_, source.data(), source.size()), "write");
            if (written && written.value() < source.size())
                return core::makeError(core::ErrorCode::OutOfSpace, "short write");
            return written;
        }
        std::size_t done = 0;
        while (done < source.size()) {
            const std::lock_guard lock{shared.mutex};
            const std::size_t pad = shared.padFor(append_ ? endOfFile() : position());
            const std::size_t count = std::min(source.size() - done, kBounceBytes - shared.alignment);
            std::memcpy(shared.data + pad, source.data() + done, count);
            const ssize_t written = ::write(fd_, shared.data + pad, count);
            if (written < 0) return errnoError("write");
            if (static_cast<std::size_t>(written) < count)
                return core::makeError(core::ErrorCode::OutOfSpace, "short write");
            done += count;
        }
        return done;
    }

    core::Result<std::uint64_t> seek(std::int64_t offset, hal::SeekOrigin origin) override {
        const int whence = origin == hal::SeekOrigin::Begin     ? SEEK_SET
                           : origin == hal::SeekOrigin::Current ? SEEK_CUR
                                                                : SEEK_END;
        const off_t at = ::lseek(fd_, static_cast<off_t>(offset), whence);
        if (at < 0) return errnoError("lseek");
        return static_cast<std::uint64_t>(at);
    }

    core::Result<std::uint64_t> tell() const override {
        const off_t at = ::lseek(fd_, 0, SEEK_CUR);
        if (at < 0) return errnoError("lseek");
        return static_cast<std::uint64_t>(at);
    }

    core::Result<std::uint64_t> size() const override {
        struct stat info {};
        if (::fstat(fd_, &info) != 0) return errnoError("fstat");
        return static_cast<std::uint64_t>(info.st_size);
    }

    core::Status flush() override {
        if (::fsync(fd_) != 0) return errnoError("fsync");
        return core::success();
    }

private:
    static core::Result<std::size_t> direct(ssize_t result, std::string_view what) {
        if (result < 0) return errnoError(what);
        return static_cast<std::size_t>(result);
    }
    [[nodiscard]] std::uint64_t position() const {
        const off_t at = ::lseek(fd_, 0, SEEK_CUR);
        return at < 0 ? 0 : static_cast<std::uint64_t>(at);
    }
    [[nodiscard]] std::uint64_t endOfFile() const {
        struct stat info {};
        return ::fstat(fd_, &info) == 0 ? static_cast<std::uint64_t>(info.st_size) : 0;
    }

    int fd_;
    bool append_;
};

int openFlags(hal::OpenMode mode) {
    switch (mode) {
        case hal::OpenMode::Read: return O_RDONLY;
        case hal::OpenMode::Write: return O_WRONLY | O_CREAT | O_TRUNC;
        case hal::OpenMode::ReadWrite: return O_RDWR;
        case hal::OpenMode::Append: return O_WRONLY | O_CREAT | O_APPEND;
    }
    return O_RDONLY;
}

core::Error fromErrno(int error, const core::Path& path) {
    core::ErrorCode code = core::ErrorCode::IoError;
    if (error == ENOENT) code = core::ErrorCode::NotFound;
    if (error == EEXIST) code = core::ErrorCode::AlreadyExists;
    if (error == EACCES || error == EROFS) code = core::ErrorCode::PermissionDenied;
    if (error == ENOSPC) code = core::ErrorCode::OutOfSpace;
    return core::makeError(code, path.string() + ": " + std::strerror(error));
}

bool statPath(const core::Path& path, struct stat& info) {
    return ::stat(path.c_str(), &info) == 0;
}

core::Path withSuffix(const core::Path& path, const char* suffix) {
    core::Path result = path;
    result += suffix;
    return result;
}

esp_err_t tryMount(const SdCardConfig& config, bool useLdo, sdmmc_card_t** card) {
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = config.maxFrequencyKhz;
    sd_pwr_ctrl_handle_t power = nullptr;
    if (useLdo) {
        sd_pwr_ctrl_ldo_config_t ldo{};
        ldo.ldo_chan_id = config.ldoChannel;
        if (const esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo, &power); err != ESP_OK)
            return err;
        host.pwr_ctrl_handle = power;
    }

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = static_cast<gpio_num_t>(config.clk);
    slot.cmd = static_cast<gpio_num_t>(config.cmd);
    slot.d0 = static_cast<gpio_num_t>(config.d0);
    slot.d1 = static_cast<gpio_num_t>(config.d1);
    slot.d2 = static_cast<gpio_num_t>(config.d2);
    slot.d3 = static_cast<gpio_num_t>(config.d3);
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount{};
    mount.format_if_mount_failed = false;  // Never erase someone's card.
    mount.max_files = 8;
    mount.allocation_unit_size = 16 * 1024;

    const esp_err_t err = esp_vfs_fat_sdmmc_mount(config.mountPoint, &host, &slot, &mount, card);
    if (err != ESP_OK && power != nullptr) sd_pwr_ctrl_del_on_chip_ldo(power);
    return err;
}

}  // namespace

core::Result<std::unique_ptr<FatFsStorage>> FatFsStorage::mount(const SdCardConfig& config,
                                                                core::Path projectsRoot) {
    sdmmc_card_t* card = nullptr;
    esp_err_t err = config.ldoChannel >= 0 ? tryMount(config, true, &card) : ESP_FAIL;
    if (err != ESP_OK) {
        if (config.ldoChannel >= 0) {
            core::log::warning(kModule, std::string{"mount with LDO power failed ("} +
                                            esp_err_to_name(err) + "), retrying without");
        }
        err = tryMount(config, false, &card);
    }
    if (err != ESP_OK && config.maxFrequencyKhz > SDMMC_FREQ_DEFAULT) {
        core::log::warning(kModule, std::string{"mount at "} + std::to_string(config.maxFrequencyKhz / 1000) +
                                        " MHz failed (" + esp_err_to_name(err) + "), retrying at 20 MHz");
        SdCardConfig slower = config;
        slower.maxFrequencyKhz = SDMMC_FREQ_DEFAULT;
        err = tryMount(slower, slower.ldoChannel >= 0, &card);
    }
    if (err != ESP_OK) {
        return core::makeError(core::ErrorCode::DeviceNotConnected,
                               std::string{"SD card not mounted: "} + esp_err_to_name(err));
    }

    const std::uint64_t bytes =
        static_cast<std::uint64_t>(card->csd.capacity) * static_cast<std::uint64_t>(card->csd.sector_size);
    core::log::info(kModule, std::string{"card '"} + card->cid.name + "', " +
                                 std::to_string(bytes >> 20) + " MiB, " +
                                 std::to_string(card->real_freq_khz / 1000) + " MHz, mounted at " +
                                 config.mountPoint);
    auto storage = std::unique_ptr<FatFsStorage>{
        new FatFsStorage{config.mountPoint, card, std::move(projectsRoot)}};
    // The shared transfer buffer, now, while internal RAM is unfragmented.
    if (bounce().data == nullptr)
        core::log::warning(kModule, "no internal DMA buffer; large transfers will be slow");
    if (auto free = storage->freeSpaceBytes(storage->projectsRoot()); free) {
        core::log::info(kModule, std::to_string(free.value() >> 20) + " MiB free");
    } else {
        core::log::warning(kModule, free.error().describe());
    }
    return storage;
}

FatFsStorage::FatFsStorage(const char* mountPoint, void* card, core::Path projectsRoot)
    : mountPoint_{mountPoint}, card_{card}, projectsRoot_{std::move(projectsRoot)} {}

FatFsStorage::~FatFsStorage() {
    esp_vfs_fat_sdcard_unmount(mountPoint_, static_cast<sdmmc_card_t*>(card_));
}

void FatFsStorage::recover(const core::Path& path) const {
    const core::Path old = withSuffix(path, ".old");
    struct stat info {};
    if (!statPath(old, info) || S_ISDIR(info.st_mode)) return;
    if (statPath(path, info)) {
        ::unlink(old.c_str());  // Step 3 never ran; the new version is in place.
    } else if (::rename(old.c_str(), path.c_str()) == 0) {
        core::log::warning(kModule, "restored " + path.string() + " after an interrupted save");
    }
}

core::Result<std::unique_ptr<hal::FileHandle>> FatFsStorage::open(const core::Path& path,
                                                                  hal::OpenMode mode) {
    recover(path);
    int fd = ::open(path.c_str(), openFlags(mode), 0644);
    if (fd < 0 && mode == hal::OpenMode::ReadWrite) {
        fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);  // As "w+b" after "r+b".
    }
    if (fd < 0) return fromErrno(errno, path);
    return std::unique_ptr<hal::FileHandle>{
        std::make_unique<FatFsFile>(fd, mode == hal::OpenMode::Append)};
}

bool FatFsStorage::exists(const core::Path& path) const {
    recover(path);
    struct stat info {};
    return statPath(path, info);
}

bool FatFsStorage::isDirectory(const core::Path& path) const {
    recover(path);
    struct stat info {};
    return statPath(path, info) && S_ISDIR(info.st_mode);
}

core::Status FatFsStorage::createDirectories(const core::Path& path) {
    core::Path partial;
    for (const auto& part : path) {
        partial /= part;
        if (partial == partial.root_path()) continue;
        struct stat info {};
        if (statPath(partial, info)) {
            if (!S_ISDIR(info.st_mode)) return fromErrno(ENOTDIR, partial);
            continue;
        }
        if (::mkdir(partial.c_str(), 0775) != 0 && errno != EEXIST) return fromErrno(errno, partial);
    }
    return core::success();
}

core::Status FatFsStorage::remove(const core::Path& path) {
    struct stat info {};
    if (!statPath(path, info)) return core::success();  // Like remove_all: nothing to do.
    if (!S_ISDIR(info.st_mode)) {
        return ::unlink(path.c_str()) == 0 ? core::success() : core::Status{fromErrno(errno, path)};
    }
    auto children = list(path);
    if (!children) return children.error();
    for (const auto& child : children.value()) {
        if (auto status = remove(path / child.name); !status) return status;
    }
    return ::rmdir(path.c_str()) == 0 ? core::success() : core::Status{fromErrno(errno, path)};
}

core::Status FatFsStorage::rename(const core::Path& from, const core::Path& to) {
    struct stat info {};
    if (!statPath(from, info)) return fromErrno(ENOENT, from);
    if (!statPath(to, info)) {
        return ::rename(from.c_str(), to.c_str()) == 0 ? core::success()
                                                       : core::Status{fromErrno(errno, from)};
    }
    if (S_ISDIR(info.st_mode)) return fromErrno(EISDIR, to);

    const core::Path old = withSuffix(to, ".old");
    ::unlink(old.c_str());  // A leftover from an earlier interrupted save.
    if (::rename(to.c_str(), old.c_str()) != 0) return fromErrno(errno, to);
    if (::rename(from.c_str(), to.c_str()) != 0) {
        const int error = errno;
        ::rename(old.c_str(), to.c_str());  // Put the previous version back.
        return fromErrno(error, from);
    }
    ::unlink(old.c_str());
    return core::success();
}

core::Result<std::vector<hal::DirectoryEntry>> FatFsStorage::list(
    const core::Path& directory) const {
    DIR* dir = ::opendir(directory.c_str());
    if (dir == nullptr) return fromErrno(errno, directory);
    std::vector<hal::DirectoryEntry> entries;
    while (const dirent* entry = ::readdir(dir)) {
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
        hal::DirectoryEntry item;
        item.name = entry->d_name;
        struct stat info {};
        if (statPath(directory / item.name, info)) {
            item.isDirectory = S_ISDIR(info.st_mode);
            item.size = item.isDirectory ? 0 : static_cast<std::uint64_t>(info.st_size);
            item.modifiedUnixSeconds = static_cast<std::int64_t>(info.st_mtime);
        }
        entries.push_back(std::move(item));
    }
    ::closedir(dir);
    return entries;
}

core::Result<std::uint64_t> FatFsStorage::freeSpaceBytes(const core::Path& /*path*/) const {
    std::uint64_t total = 0;
    std::uint64_t free = 0;
    if (const esp_err_t err = esp_vfs_fat_info(mountPoint_, &total, &free); err != ESP_OK) {
        return core::makeError(core::ErrorCode::IoError,
                               std::string{"esp_vfs_fat_info: "} + esp_err_to_name(err));
    }
    return free;
}

}  // namespace aw::platform::esp32p4
