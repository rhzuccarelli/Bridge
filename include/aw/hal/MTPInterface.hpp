#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "aw/core/Result.hpp"

namespace aw::hal {

/// Opaque handle to an object (file or folder) on an MTP device.
/// Valid only for the lifetime of the session that produced it.
using MtpObjectHandle = std::uint32_t;

/// Logical volume on the device.  The TP-7 exposes one.
using MtpStorageId = std::uint32_t;

inline constexpr MtpObjectHandle kMtpRootHandle = 0xFFFFFFFFu;  ///< Parent of top level objects.

struct MtpDeviceInfo {
    std::string manufacturer;
    std::string model;
    std::string serialNumber;
    std::string firmwareVersion;
};

struct MtpStorageInfo {
    MtpStorageId id{0};
    std::string description;
    std::uint64_t capacityBytes{0};
    std::uint64_t freeBytes{0};
};

struct MtpObjectInfo {
    MtpObjectHandle handle{0};
    MtpObjectHandle parent{kMtpRootHandle};
    MtpStorageId storageId{0};
    std::string name;
    std::uint64_t sizeBytes{0};
    bool isFolder{false};
    std::int64_t modifiedUnixSeconds{0};
};

/// Receives an object's bytes during a download.
/// Return false to abort the transfer (user pressed Stop).
using MtpWriteSink = std::function<bool(std::span<const std::byte> chunk)>;

/// Supplies an object's bytes during an upload.  Returns the number of bytes
/// written into `buffer`; 0 means end of data.
using MtpReadSource = std::function<std::size_t(std::span<std::byte> buffer)>;

/// Progress notification.  Return false to cancel.
using MtpProgressCallback = std::function<bool(std::uint64_t done, std::uint64_t total)>;

/// Media Transfer Protocol initiator.
///
/// Why MTP gets its own HAL interface instead of being hidden behind
/// `StorageInterface`: MTP is not a filesystem and pretending otherwise is how
/// these integrations go wrong.  There are no paths, only handles.  There is no
/// seek, no partial write, no rename-in-place, no atomic replace.  Objects are
/// transferred whole, sizes must be declared up front, and the device may
/// renumber handles between sessions.  A `StorageInterface` that had to satisfy
/// both a real SD card and this would be a lowest-common-denominator API that
/// serves neither.
///
/// Threading: every call here blocks, sometimes for seconds.  All of it runs on
/// the I/O worker thread.  The UI learns about progress through events.
class MTPInterface {
public:
    virtual ~MTPInterface() = default;

    MTPInterface(const MTPInterface&) = delete;
    MTPInterface& operator=(const MTPInterface&) = delete;

    /// `deviceKey` comes from `UsbDeviceInfo::key()`.
    [[nodiscard]] virtual core::Status openSession(std::uint32_t deviceKey) = 0;
    virtual void closeSession() noexcept = 0;
    [[nodiscard]] virtual bool hasSession() const noexcept = 0;

    [[nodiscard]] virtual core::Result<MtpDeviceInfo> deviceInfo() const = 0;
    [[nodiscard]] virtual core::Result<std::vector<MtpStorageInfo>> storages() const = 0;

    /// Immediate children of `parent`.  Not recursive: the browser walks one
    /// level at a time so a deep tree never blocks the UI for long.
    [[nodiscard]] virtual core::Result<std::vector<MtpObjectInfo>> listObjects(
        MtpStorageId storage, MtpObjectHandle parent) = 0;

    [[nodiscard]] virtual core::Result<MtpObjectInfo> objectInfo(MtpObjectHandle handle) = 0;

    /// Streams an object off the device into `sink`.
    [[nodiscard]] virtual core::Status readObject(MtpObjectHandle handle,
                                                  const MtpWriteSink& sink,
                                                  const MtpProgressCallback& progress) = 0;

    /// Streams a new object onto the device.  `sizeBytes` must be exact - MTP
    /// requires the size in SendObjectInfo before any data moves.
    /// Never overwrites: fails with AlreadyExists if `name` is taken under
    /// `parent`.  Replacing is an explicit delete-then-write by the caller,
    /// after the user confirms.
    [[nodiscard]] virtual core::Result<MtpObjectHandle> writeObject(
        MtpStorageId storage,
        MtpObjectHandle parent,
        const std::string& name,
        std::uint64_t sizeBytes,
        const MtpReadSource& source,
        const MtpProgressCallback& progress) = 0;

    [[nodiscard]] virtual core::Status deleteObject(MtpObjectHandle handle) = 0;

protected:
    MTPInterface() = default;
};

}  // namespace aw::hal
