#pragma once

#include <string>
#include <utility>

#include "aw/hal/DeviceModeInterface.hpp"

namespace aw::platform::rpi {

/// Sends the TP-7 mode SysEx through its ALSA raw-MIDI output.
class AlsaTp7ModeSwitcher final : public hal::DeviceModeInterface {
public:
    explicit AlsaTp7ModeSwitcher(std::string device) : device_{std::move(device)} {}

    [[nodiscard]] core::Status requestMtpMode() override;

private:
    std::string device_;
};

}  // namespace aw::platform::rpi
