#pragma once

#include <memory>

#include "aw/hal/MTPInterface.hpp"

namespace aw::platform::rpi {

/// MTP initiator backed by libmtp.  (Phase 8.)
///
/// Why libmtp rather than writing PTP/MTP over libusb by hand: MTP has enough
/// device-specific quirks that a mature implementation is worth the dependency
/// on a platform that can afford it.  The ESP32-P4 cannot - it will get a
/// minimal initiator over TinyUSB implementing only the eight operations
/// `hal::MTPInterface` exposes.  That is precisely why the interface is small:
/// it is sized to what a hand-written initiator can reasonably do.
///
/// Streaming: libmtp's callback-based get/send file APIs are wired straight to
/// `MtpWriteSink`/`MtpReadSource`, so a 200 MB take never lands in RAM.
class LibMtpDevice final : public hal::MTPInterface {
public:
    LibMtpDevice();
    ~LibMtpDevice() override;

    core::Status openSession(std::uint32_t deviceKey) override;
    void closeSession() noexcept override;
    [[nodiscard]] bool hasSession() const noexcept override;

    [[nodiscard]] core::Result<hal::MtpDeviceInfo> deviceInfo() const override;
    [[nodiscard]] core::Result<std::vector<hal::MtpStorageInfo>> storages() const override;

    [[nodiscard]] core::Result<std::vector<hal::MtpObjectInfo>> listObjects(
        hal::MtpStorageId storage, hal::MtpObjectHandle parent) override;
    [[nodiscard]] core::Result<hal::MtpObjectInfo> objectInfo(
        hal::MtpObjectHandle handle) override;

    [[nodiscard]] core::Status readObject(hal::MtpObjectHandle handle,
                                          const hal::MtpWriteSink& sink,
                                          const hal::MtpProgressCallback& progress) override;
    [[nodiscard]] core::Result<hal::MtpObjectHandle> writeObject(
        hal::MtpStorageId storage,
        hal::MtpObjectHandle parent,
        const std::string& name,
        std::uint64_t sizeBytes,
        const hal::MtpReadSource& source,
        const hal::MtpProgressCallback& progress) override;

    [[nodiscard]] core::Status deleteObject(hal::MtpObjectHandle handle) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aw::platform::rpi
