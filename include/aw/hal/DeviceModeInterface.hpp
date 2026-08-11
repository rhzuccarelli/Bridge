#pragma once

#include "aw/core/Result.hpp"

namespace aw::hal {

/// Optional device-specific mode control.
///
/// The TP-7 accepts a Teenage Engineering SysEx request while it is exposing
/// USB audio/MIDI, then re-enumerates as MTP. Returning to audio is handled by
/// closing the MTP session, so only the forward transition belongs here.
class DeviceModeInterface {
public:
    virtual ~DeviceModeInterface() = default;

    DeviceModeInterface(const DeviceModeInterface&) = delete;
    DeviceModeInterface& operator=(const DeviceModeInterface&) = delete;

    [[nodiscard]] virtual core::Status requestMtpMode() = 0;

protected:
    DeviceModeInterface() = default;
};

}  // namespace aw::hal
