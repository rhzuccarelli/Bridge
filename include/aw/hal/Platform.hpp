#pragma once

#include <memory>

#include "aw/hal/AudioInputInterface.hpp"
#include "aw/hal/AudioOutputInterface.hpp"
#include "aw/hal/ButtonInterface.hpp"
#include "aw/hal/ClockInterface.hpp"
#include "aw/hal/DisplayInterface.hpp"
#include "aw/hal/EncoderInterface.hpp"
#include "aw/hal/MTPInterface.hpp"
#include "aw/hal/StorageInterface.hpp"
#include "aw/hal/USBInterface.hpp"

namespace aw::hal {

/// Everything the application needs from the hardware, in one owning bundle.
///
/// This is the object the composition root builds and the only place where a
/// concrete backend name appears.  `Application` receives a `Platform&` and can
/// therefore be constructed against real hardware, against the simulator, or
/// against test doubles without a single `#ifdef`.
///
/// `audioInput` and `mtp` may be null: a Pi with no capture device and no USB
/// device attached is a valid, usable workstation.  Everything else is required
/// and the factory fails if it cannot be built.
struct Platform {
    std::unique_ptr<ClockInterface> clock;
    std::unique_ptr<StorageInterface> storage;
    std::unique_ptr<DisplayInterface> display;
    std::unique_ptr<ButtonInterface> buttons;
    std::unique_ptr<EncoderInterface> encoder;
    std::unique_ptr<AudioOutputInterface> audioOutput;
    std::unique_ptr<AudioInputInterface> audioInput;  ///< Optional.
    std::unique_ptr<USBInterface> usb;
    std::unique_ptr<MTPInterface> mtp;                ///< Optional.

    [[nodiscard]] bool isComplete() const noexcept {
        return clock && storage && display && buttons && encoder && audioOutput && usb;
    }
};

}  // namespace aw::hal
