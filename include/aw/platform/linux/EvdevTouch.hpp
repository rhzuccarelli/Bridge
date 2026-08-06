#pragma once

#include <memory>
#include <string>

#include "aw/hal/ButtonInterface.hpp"
#include "aw/platform/desktop/VirtualPanel.hpp"

namespace aw::platform::linuxfb {

struct TouchConfig {
    /// Empty means "scan /dev/input/event* and pick the first absolute-position
    /// device", which is what an XPT2046 registers as once the kernel driver
    /// (ads7846 / xpt2046, via a device-tree overlay) is loaded.
    std::string device;

    /// Physical panel size, used to scale raw ADC coordinates.
    std::uint16_t panelWidth{480};
    std::uint16_t panelHeight{320};

    /// Resistive panels are frequently mounted rotated relative to the display.
    bool swapAxes{false};
    bool invertX{false};
    bool invertY{false};
};

/// A resistive touchscreen presented as a `ButtonInterface`.
///
/// It is a button backend, not a pointer backend, and that is the entire point.
/// The product has no touchscreen; the UI must never learn about coordinates,
/// drags or gestures, or the interaction design will quietly become
/// touch-shaped and then fail to port to a device with a knob and eight
/// switches.  So this class hit-tests a tap against `VirtualPanelLayout` and
/// emits `ButtonId` - the same events a real switch produces, indistinguishable
/// upstream.
///
/// Reading evdev rather than driving the XPT2046 over SPI ourselves: the kernel
/// driver already does the ADC sampling, pressure thresholding and debouncing,
/// and its calibration is configurable through the usual userspace tools. Doing
/// it by hand would be worse in every respect.
///
/// Touch coordinates outside the key strip - i.e. taps on the UI canvas itself -
/// are discarded. Deliberately: the canvas is not touchable on the real device.
class EvdevTouch final : public hal::ButtonInterface {
public:
    /// Takes the layout **by value**, and the display backend is given a copy of
    /// the same one.  Passing it rather than letting each side derive its own
    /// removes the "these two must agree" invariant entirely: hit testing is
    /// guaranteed to match what was drawn, by construction.
    EvdevTouch(TouchConfig config, desktop::VirtualPanelLayout layout);
    ~EvdevTouch() override;

    void setListener(hal::ButtonListener* listener) noexcept override;
    core::Status start() override;
    void stop() noexcept override;
    [[nodiscard]] bool isPressed(core::ButtonId id) const noexcept override;

    /// Path of the device actually opened, for logging and diagnostics.
    [[nodiscard]] std::string devicePath() const;

    /// Finds a touch device without opening a session; empty when none exists.
    [[nodiscard]] static std::string findTouchDevice();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aw::platform::linuxfb
