#pragma once

#include <optional>
#include <string>
#include <vector>

#include "aw/core/Result.hpp"
#include "aw/events/EventQueue.hpp"
#include "aw/hal/MTPInterface.hpp"
#include "aw/hal/USBInterface.hpp"

namespace aw::mtp {

/// Connection state, as the UI understands it.
enum class ConnectionState {
    Disconnected,   ///< No MTP capable device on the bus.
    Detected,       ///< Device seen, session not open yet.
    Connected,      ///< Session open, browsable.
    Error,          ///< Session attempt failed; the reason is in `lastError`.
};

/// Owns the device connection lifecycle.
///
/// This is the class that turns "someone plugged a cable in" into something the
/// application can reason about.  It listens for hotplug, decides whether the
/// device is one we can talk to, opens and closes the MTP session, and posts
/// `events::DeviceEvent`.
///
/// What it deliberately does not do: transfer files (that is Import/Export
/// Service), or open a session automatically the moment a device appears
/// beyond the cheap identification step.  The TP-7 is someone's recorder; the
/// device connects when the user asks to browse it, and never touches its
/// contents otherwise.
class MtpManager : public hal::UsbHotplugListener {
public:
    MtpManager(hal::USBInterface& usb, hal::MTPInterface& mtp, events::EventQueue& eventQueue);
    ~MtpManager() override;

    MtpManager(const MtpManager&) = delete;
    MtpManager& operator=(const MtpManager&) = delete;

    /// Starts hotplug watching and does one enumeration pass, so a device that
    /// was already plugged in at boot is noticed.
    [[nodiscard]] core::Status start();
    void stop() noexcept;

    [[nodiscard]] ConnectionState state() const noexcept { return state_; }
    [[nodiscard]] const core::Error& lastError() const noexcept { return lastError_; }

    /// Opens a session with the detected device.  Blocking; I/O thread only.
    [[nodiscard]] core::Status connect();
    void disconnect() noexcept;

    [[nodiscard]] std::optional<hal::MtpDeviceInfo> deviceInfo() const { return deviceInfo_; }
    [[nodiscard]] core::Result<std::vector<hal::MtpStorageInfo>> storages() const;

    // hal::UsbHotplugListener
    void onUsbDeviceAttached(const hal::UsbDeviceInfo& device) override;
    void onUsbDeviceDetached(const hal::UsbDeviceInfo& device) override;

private:
    hal::USBInterface& usb_;
    hal::MTPInterface& mtp_;
    events::EventQueue& eventQueue_;

    ConnectionState state_{ConnectionState::Disconnected};
    core::Error lastError_;
    std::optional<hal::UsbDeviceInfo> detectedDevice_;
    std::optional<hal::MtpDeviceInfo> deviceInfo_;
};

}  // namespace aw::mtp
