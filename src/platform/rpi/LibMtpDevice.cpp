#include "aw/platform/rpi/LibMtpDevice.hpp"

// Phase 8 replaces these stubs with libmtp calls.

namespace aw::platform::rpi {
namespace {
core::Error pending(const char* what) {
    return core::notImplemented(std::string{what} + " (Phase 8)");
}
}  // namespace

struct LibMtpDevice::Impl {
    bool sessionOpen{false};
};

LibMtpDevice::LibMtpDevice() : impl_{std::make_unique<Impl>()} {}
LibMtpDevice::~LibMtpDevice() = default;

core::Status LibMtpDevice::openSession(std::uint32_t /*deviceKey*/) {
    return pending("LibMtpDevice::openSession");
}

void LibMtpDevice::closeSession() noexcept { impl_->sessionOpen = false; }

bool LibMtpDevice::hasSession() const noexcept { return impl_->sessionOpen; }

core::Result<hal::MtpDeviceInfo> LibMtpDevice::deviceInfo() const {
    return pending("LibMtpDevice::deviceInfo");
}

core::Result<std::vector<hal::MtpStorageInfo>> LibMtpDevice::storages() const {
    return pending("LibMtpDevice::storages");
}

core::Result<std::vector<hal::MtpObjectInfo>> LibMtpDevice::listObjects(
    hal::MtpStorageId /*storage*/, hal::MtpObjectHandle /*parent*/) {
    return pending("LibMtpDevice::listObjects");
}

core::Result<hal::MtpObjectInfo> LibMtpDevice::objectInfo(hal::MtpObjectHandle /*handle*/) {
    return pending("LibMtpDevice::objectInfo");
}

core::Status LibMtpDevice::readObject(hal::MtpObjectHandle /*handle*/,
                                      const hal::MtpWriteSink& /*sink*/,
                                      const hal::MtpProgressCallback& /*progress*/) {
    return pending("LibMtpDevice::readObject");
}

core::Result<hal::MtpObjectHandle> LibMtpDevice::writeObject(
    hal::MtpStorageId /*storage*/,
    hal::MtpObjectHandle /*parent*/,
    const std::string& /*name*/,
    std::uint64_t /*sizeBytes*/,
    const hal::MtpReadSource& /*source*/,
    const hal::MtpProgressCallback& /*progress*/) {
    return pending("LibMtpDevice::writeObject");
}

core::Status LibMtpDevice::deleteObject(hal::MtpObjectHandle /*handle*/) {
    return pending("LibMtpDevice::deleteObject");
}

}  // namespace aw::platform::rpi
