#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "aw/core/Result.hpp"
#include "aw/hal/UsbControlInterface.hpp"

namespace aw::devices {

/// What an analogue input of the Duet expects. The values are the Duet's own.
enum class DuetInputType : std::uint8_t {
    Line4dBu = 0,
    Line10dBV = 1,
    Microphone = 2,
    Instrument = 3,
};

inline constexpr int kDuetInputTypeCount = 4;

/// Short name for the screen: "+4DBU", "-10DBV", "MIC", "INST".
[[nodiscard]] std::string_view toString(DuetInputType type) noexcept;

/// The settings of an Apogee Duet 2 (USB), as vendor requests on endpoint 0.
///
/// The protocol is the one Apogee's Maestro app uses, as worked out by
/// take_control (github.com/stefanocoding/take_control): every setting is one
/// byte, read with request type 0xC0 and written with 0x40, wValue 0 and the
/// input number (0 or 1) as wIndex. take_control targets the later, USB 2.0
/// compliant Duet 2 (product 0x0016); the original one (0x0007) answers the
/// input settings the same way.
///
/// Blocking, like the interface under it: use it from the I/O worker.
class ApogeeDuet {
public:
    static constexpr std::uint16_t kVendorId = 0x0c60;
    static constexpr std::uint16_t kCompliantProductId = 0x0016;
    static constexpr std::uint16_t kOriginalProductId = 0x0007;
    static constexpr int kInputCount = 2;

    explicit ApogeeDuet(hal::UsbControlInterface& usb) noexcept : usb_{usb} {}

    /// `input` is 0 for input 1, 1 for input 2.
    [[nodiscard]] core::Result<DuetInputType> inputType(int input);

    /// Ungroups the inputs first, as Maestro does: grouped inputs share their
    /// settings, and changing one input's type would otherwise leave the pair
    /// in a state the Duet's own controls do not expect.
    [[nodiscard]] core::Status setInputType(int input, DuetInputType type);

private:
    static constexpr std::uint8_t kRead = 0xc0;
    static constexpr std::uint8_t kWrite = 0x40;
    static constexpr std::uint8_t kInputTypeRequest = 22;
    static constexpr std::uint8_t kGroupRequest = 68;

    [[nodiscard]] core::Result<std::uint8_t> read(std::uint8_t request, int input);
    [[nodiscard]] core::Status write(std::uint8_t request, int input, std::uint8_t value);
    /// Sends to whichever Duet 2 is attached, the last one found first.
    [[nodiscard]] core::Result<std::size_t> control(const hal::UsbControlSetup& setup,
                                                    std::span<std::uint8_t> data);

    hal::UsbControlInterface& usb_;
    std::uint16_t productId_{kCompliantProductId};
};

}  // namespace aw::devices
