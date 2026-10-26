#include "aw/devices/ApogeeDuet.hpp"

#include <array>
#include <string>

namespace aw::devices {
namespace {

core::Status checkInput(int input) {
    if (input < 0 || input >= ApogeeDuet::kInputCount)
        return core::makeError(core::ErrorCode::InvalidArgument,
                               "Duet input " + std::to_string(input + 1));
    return core::success();
}

}  // namespace

std::string_view toString(DuetInputType type) noexcept {
    switch (type) {
        case DuetInputType::Line4dBu: return "+4DBU";
        case DuetInputType::Line10dBV: return "-10DBV";
        case DuetInputType::Microphone: return "MIC";
        case DuetInputType::Instrument: return "INST";
    }
    return "?";
}

core::Result<DuetInputType> ApogeeDuet::inputType(int input) {
    auto value = read(kInputTypeRequest, input);
    if (!value) return value.error();
    if (value.value() >= kDuetInputTypeCount)
        return core::makeError(core::ErrorCode::ProtocolError,
                               "Duet input type " + std::to_string(value.value()));
    return static_cast<DuetInputType>(value.value());
}

core::Status ApogeeDuet::setInputType(int input, DuetInputType type) {
    if (auto status = checkInput(input); !status) return status;
    for (int each = 0; each < kInputCount; ++each) {
        if (auto status = write(kGroupRequest, each, 0); !status) return status;
    }
    return write(kInputTypeRequest, input, static_cast<std::uint8_t>(type));
}

core::Result<std::uint8_t> ApogeeDuet::read(std::uint8_t request, int input) {
    if (auto status = checkInput(input); !status) return status.error();
    std::array<std::uint8_t, 1> value{};
    auto got = control({kRead, request, 0, static_cast<std::uint16_t>(input)}, value);
    if (!got) return got.error();
    if (got.value() != value.size())
        return core::makeError(core::ErrorCode::ProtocolError, "short reply from the Duet");
    return value[0];
}

core::Status ApogeeDuet::write(std::uint8_t request, int input, std::uint8_t value) {
    std::array<std::uint8_t, 1> data{value};
    auto sent = control({kWrite, request, 0, static_cast<std::uint16_t>(input)}, data);
    if (!sent) return sent.error();
    return core::success();
}

core::Result<std::size_t> ApogeeDuet::control(const hal::UsbControlSetup& setup,
                                              std::span<std::uint8_t> data) {
    auto result = usb_.control(kVendorId, productId_, setup, data);
    if (result || result.error().code() != core::ErrorCode::DeviceNotConnected) return result;
    const std::uint16_t other =
        productId_ == kCompliantProductId ? kOriginalProductId : kCompliantProductId;
    auto retried = usb_.control(kVendorId, other, setup, data);
    if (retried) productId_ = other;
    if (retried || retried.error().code() != core::ErrorCode::DeviceNotConnected) return retried;
    return result;
}

}  // namespace aw::devices
