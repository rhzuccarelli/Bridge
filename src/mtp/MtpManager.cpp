#include "aw/mtp/MtpManager.hpp"

#include "aw/core/Logger.hpp"

namespace aw::mtp {
namespace {
constexpr std::string_view kModule = "mtp";
}

MtpManager::MtpManager(hal::USBInterface& usb,
                       hal::MTPInterface& mtp,
                       events::EventQueue& eventQueue)
    : usb_{usb}, mtp_{mtp}, eventQueue_{eventQueue} {}

MtpManager::~MtpManager() { stop(); }

core::Status MtpManager::start() {
    usb_.setHotplugListener(this);
    if (auto status = usb_.start(); !status) {
        return status;
    }

    // Catch devices that were already attached before we started listening.
    auto devices = usb_.enumerate();
    if (!devices) {
        return devices.error();
    }
    for (const hal::UsbDeviceInfo& device : devices.value()) {
        if (device.supportsMtp) {
            onUsbDeviceAttached(device);
            break;  // One device is all this workflow supports.
        }
    }
    return core::success();
}

void MtpManager::stop() noexcept {
    disconnect();
    usb_.setHotplugListener(nullptr);
    usb_.stop();
}

core::Status MtpManager::connect() {
    if (!detectedDevice_) {
        return core::makeError(core::ErrorCode::DeviceNotConnected, "no MTP device detected");
    }
    if (state_ == ConnectionState::Connected) {
        return core::success();
    }

    if (auto status = mtp_.openSession(detectedDevice_->key()); !status) {
        state_ = ConnectionState::Error;
        lastError_ = status.error();
        return status;
    }

    auto info = mtp_.deviceInfo();
    if (!info) {
        mtp_.closeSession();
        state_ = ConnectionState::Error;
        lastError_ = info.error();
        return info.error();
    }

    deviceInfo_ = std::move(info).value();
    state_ = ConnectionState::Connected;
    core::log::info(kModule, "MTP session open: " + deviceInfo_->model);
    return core::success();
}

void MtpManager::disconnect() noexcept {
    if (mtp_.hasSession()) {
        mtp_.closeSession();
    }
    deviceInfo_.reset();
    if (state_ == ConnectionState::Connected) {
        state_ = detectedDevice_ ? ConnectionState::Detected : ConnectionState::Disconnected;
    }
}

core::Result<std::vector<hal::MtpStorageInfo>> MtpManager::storages() const {
    if (state_ != ConnectionState::Connected) {
        return core::makeError(core::ErrorCode::InvalidState, "no MTP session");
    }
    return mtp_.storages();
}

void MtpManager::onUsbDeviceAttached(const hal::UsbDeviceInfo& device) {
    if (!device.supportsMtp || detectedDevice_) {
        return;
    }
    detectedDevice_ = device;
    state_ = ConnectionState::Detected;
    core::log::info(kModule, "MTP device attached: " + device.product);
    eventQueue_.post(events::DeviceEvent{events::DeviceKind::Mtp, true, device.key()});
}

void MtpManager::onUsbDeviceDetached(const hal::UsbDeviceInfo& device) {
    if (!detectedDevice_ || detectedDevice_->key() != device.key()) {
        return;
    }
    // The cable is already gone; closing the session is bookkeeping, not I/O.
    if (mtp_.hasSession()) {
        mtp_.closeSession();
    }
    deviceInfo_.reset();
    detectedDevice_.reset();
    state_ = ConnectionState::Disconnected;
    core::log::info(kModule, "MTP device detached");
    eventQueue_.post(events::DeviceEvent{events::DeviceKind::Mtp, false, device.key()});
}

}  // namespace aw::mtp
