#include "aw/platform/esp32p4/FatFsStorage.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#include "aw/core/Logger.hpp"
#include "aw/platform/StdioFile.hpp"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"

namespace aw::platform::esp32p4 {
namespace {

constexpr std::string_view kModule = "sd";
constexpr std::size_t kStdioBufferBytes = 8 * 1024;  // Newlib's default is 128 bytes.

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
    return std::unique_ptr<FatFsStorage>{
        new FatFsStorage{config.mountPoint, card, std::move(projectsRoot)}};
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
    std::FILE* file = std::fopen(path.c_str(), stdioMode(mode));
    if (file == nullptr && mode == hal::OpenMode::ReadWrite) {
        file = std::fopen(path.c_str(), "w+b");
    }
    if (file == nullptr) return fromErrno(errno, path);
    std::setvbuf(file, nullptr, _IOFBF, kStdioBufferBytes);
    return std::unique_ptr<hal::FileHandle>{std::make_unique<StdioFile>(file)};
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
