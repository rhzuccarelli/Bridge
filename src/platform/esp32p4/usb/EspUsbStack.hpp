#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

#include "aw/core/Result.hpp"
#include "aw/hal/USBInterface.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "usb/usb_host.h"

namespace aw::platform::esp32p4::usb {

struct Endpoint {
    std::uint8_t address{0};
    std::uint16_t maxPacket{0};
};

/// The interfaces of a device that Bridge cares about, found by walking its
/// active configuration descriptor (the probe's classifier).
struct Layout {
    bool hasMidiOut{false};
    std::uint8_t midiInterface{0};
    std::uint8_t midiAlternate{0};
    Endpoint midiOut;
    Endpoint midiIn;  ///< Address 0 when the interface has none.

    bool hasMtp{false};
    std::uint8_t mtpInterface{0};
    std::uint8_t mtpAlternate{0};
    Endpoint bulkIn;
    Endpoint bulkOut;

    /// The first audio-streaming alternate setting with an isochronous OUT
    /// endpoint (UAC1), and the feedback endpoint its bSynchAddress names.
    struct Playback {
        bool found{false};
        std::uint8_t interface{0};
        std::uint8_t alternate{0};
        std::uint8_t endpoint{0};
        std::uint16_t maxPacket{0};        ///< Bytes per transaction.
        int transactions{1};               ///< Per microframe (wMaxPacketSize bits 12:11, plus 1).
        int intervalMicroframes{1};
        int channels{0};
        int subslot{0};                    ///< Bytes per sample.
        std::uint8_t feedbackEndpoint{0};  ///< 0 when the endpoint has none.
        std::uint16_t feedbackMaxPacket{0};
        int feedbackIntervalMicroframes{1};
    } playback;
};

/// A device borrowed for I/O. The stack keeps it open until every lease is
/// returned, even after it has been unplugged.
struct Lease {
    usb_device_handle_t handle{nullptr};
    Layout layout;
    hal::UsbDeviceInfo info;
};

/// One ESP-IDF USB host on the P4's high-speed port, shared by the
/// `USBInterface`, `MTPInterface` and `DeviceModeInterface` backends.
///
/// Three FreeRTOS tasks, so nothing ever waits on itself:
///  - the host library task handles port and enumeration events,
///  - the client task only pumps `usb_host_client_handle_events`, which is
///    where every transfer completion is delivered,
///  - the hotplug task opens new devices, classifies them and calls the
///    listener. `MtpManager` closes the MTP session from that callback; doing
///    that on the client task would deadlock on its own completion.
class EspUsbStack {
public:
    EspUsbStack() = default;
    EspUsbStack(const EspUsbStack&) = delete;
    EspUsbStack& operator=(const EspUsbStack&) = delete;

    /// Installs the host stack on first call; later calls do nothing. The host
    /// is never uninstalled: on the device it lives as long as the firmware.
    core::Status start();
    void setListener(hal::UsbHotplugListener* listener) noexcept;

    /// Backends that keep something open while a device is attached (the TP-7
    /// MIDI port). Told on the hotplug task, before the application's listener
    /// on attach and after it on detach. Add before `start()`.
    void addInternalListener(hal::UsbHotplugListener* listener);
    [[nodiscard]] std::vector<hal::UsbDeviceInfo> devices() const;

    /// Leases the first attached device that `match` accepts.
    [[nodiscard]] std::optional<Lease> acquire(
        const std::function<bool(const hal::UsbDeviceInfo&, const Layout&)>& match);
    void release(const Lease& lease) noexcept;

    /// True once the device behind `lease` has been unplugged.
    [[nodiscard]] bool isGone(const Lease& lease) const;

    [[nodiscard]] usb_host_client_handle_t client() const noexcept { return client_; }

private:
    struct Attached {
        hal::UsbDeviceInfo info;
        usb_device_handle_t handle{nullptr};
        Layout layout;
        bool gone{false};
        int leases{0};
    };
    struct Hotplug {
        bool attached{false};
        std::uint8_t address{0};
        usb_device_handle_t handle{nullptr};
    };

    static void hostTask(void* context);
    static void clientTask(void* context);
    static void hotplugTask(void* context);
    static void onClientEvent(const usb_host_client_event_msg_t* message, void* context);

    void attach(std::uint8_t address);
    void detach(usb_device_handle_t handle);
    void closeIfUnused(std::vector<Attached>::iterator device);

    mutable std::mutex mutex_;
    std::vector<Attached> devices_;
    hal::UsbHotplugListener* listener_{nullptr};
    std::vector<hal::UsbHotplugListener*> internalListeners_;
    usb_host_client_handle_t client_{nullptr};
    QueueHandle_t hotplug_{nullptr};
    bool started_{false};
};

}  // namespace aw::platform::esp32p4::usb
