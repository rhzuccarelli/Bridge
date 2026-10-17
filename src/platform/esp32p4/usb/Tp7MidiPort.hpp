#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>

#include "EspUsbStack.hpp"
#include "aw/core/Result.hpp"
#include "aw/hal/USBInterface.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace aw::platform::esp32p4::usb {

/// The TP-7's USB-MIDI interface, held for as long as a TP-7 is attached in
/// USB-audio mode.
///
/// The TP-7 sends MIDI back on its own: Start, Stop and 24 clocks per beat
/// while its transport runs, and whatever its buttons send. Nothing in Bridge
/// needs those yet, but they must be read: on the bench, once the TP-7 had
/// MIDI clock queued that nobody read, it ignored the SysEx that switches it to
/// MTP. So a read is always pending here, and what arrives is discarded.
///
/// Sending (the MTP switch, bench tooling) goes through the same claimed
/// interface.
class Tp7MidiPort final : public hal::UsbHotplugListener {
public:
    explicit Tp7MidiPort(std::shared_ptr<EspUsbStack> stack);
    ~Tp7MidiPort() override;

    /// Sends USB-MIDI event packets (4 bytes each, cable 0).
    core::Status send(std::span<const std::uint8_t> packets);

    /// USB-MIDI event packets received (and discarded) so far.
    [[nodiscard]] std::uint32_t receivedPackets() const noexcept { return received_.load(); }

    void onUsbDeviceAttached(const hal::UsbDeviceInfo& device) override;
    void onUsbDeviceDetached(const hal::UsbDeviceInfo& device) override;

private:
    static void inDone(usb_transfer_t* transfer);
    void closeLocked() noexcept;

    std::shared_ptr<EspUsbStack> stack_;
    std::mutex mutex_;
    std::optional<Lease> lease_;
    usb_transfer_t* in_{nullptr};
    SemaphoreHandle_t inStopped_{nullptr};
    std::atomic<bool> reading_{false};
    std::atomic<std::uint32_t> received_{0};
};

}  // namespace aw::platform::esp32p4::usb
