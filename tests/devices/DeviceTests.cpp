#include "aw/devices/ApogeeDuet.hpp"
#include "framework/Test.hpp"

#include <array>
#include <map>
#include <utility>
#include <vector>

using namespace aw;

namespace {

/// A Duet behind endpoint 0: one byte per (request, wIndex), and a log of
/// every request in order.
class FakeDuet final : public hal::UsbControlInterface {
public:
    core::Result<std::size_t> control(std::uint16_t vendorId, std::uint16_t productId,
                                      const hal::UsbControlSetup& setup,
                                      std::span<std::uint8_t> data) override {
        if (!attached || vendorId != devices::ApogeeDuet::kVendorId || productId != this->productId)
            return core::makeError(core::ErrorCode::DeviceNotConnected, "no Duet");
        log.push_back(setup);
        const auto key = std::pair{setup.request, setup.index};
        if ((setup.requestType & 0x80) != 0) {
            if (data.empty()) return std::size_t{0};
            data[0] = settings[key];
        } else {
            settings[key] = data.empty() ? 0 : data[0];
        }
        return data.size();
    }

    bool attached{true};
    std::uint16_t productId{devices::ApogeeDuet::kCompliantProductId};
    std::map<std::pair<std::uint8_t, std::uint16_t>, std::uint8_t> settings;
    std::vector<hal::UsbControlSetup> log;
};

}  // namespace

AW_TEST(ApogeeDuet, ReadsEachInputsType) {
    FakeDuet usb;
    usb.settings[{22, 0}] = 0;
    usb.settings[{22, 1}] = 3;
    devices::ApogeeDuet duet{usb};

    auto first = duet.inputType(0);
    auto second = duet.inputType(1);
    AW_REQUIRE(first.hasValue() && second.hasValue());
    AW_CHECK(first.value() == devices::DuetInputType::Line4dBu);
    AW_CHECK(second.value() == devices::DuetInputType::Instrument);
    AW_REQUIRE(usb.log.size() == 2);
    AW_CHECK_EQ(usb.log[0].requestType, std::uint8_t{0xc0});
    AW_CHECK_EQ(usb.log[0].value, std::uint16_t{0});
}

AW_TEST(ApogeeDuet, UngroupsTheInputsBeforeSettingAType) {
    FakeDuet usb;
    usb.settings[{68, 0}] = 1;
    usb.settings[{68, 1}] = 1;
    devices::ApogeeDuet duet{usb};

    AW_REQUIRE(duet.setInputType(0, devices::DuetInputType::Instrument).hasValue());

    AW_REQUIRE(usb.log.size() == 3);
    AW_CHECK_EQ(usb.log[0].request, std::uint8_t{68});
    AW_CHECK_EQ(usb.log[1].request, std::uint8_t{68});
    AW_CHECK_EQ(usb.log[2].requestType, std::uint8_t{0x40});
    AW_CHECK_EQ(usb.log[2].request, std::uint8_t{22});
    AW_CHECK_EQ(usb.log[2].index, std::uint16_t{0});
    AW_CHECK_EQ((usb.settings[{22, 0}]), std::uint8_t{3});
    AW_CHECK_EQ((usb.settings[{68, 0}]), std::uint8_t{0});
    AW_CHECK_EQ((usb.settings[{68, 1}]), std::uint8_t{0});
}

AW_TEST(ApogeeDuet, RejectsAnInputItDoesNotHave) {
    FakeDuet usb;
    devices::ApogeeDuet duet{usb};
    AW_CHECK(!duet.inputType(2).hasValue());
    AW_CHECK(!duet.setInputType(-1, devices::DuetInputType::Microphone).hasValue());
    AW_CHECK(usb.log.empty());
}

AW_TEST(ApogeeDuet, ReportsAnUnknownTypeAndAMissingDevice) {
    FakeDuet usb;
    usb.settings[{22, 0}] = 9;
    devices::ApogeeDuet duet{usb};
    auto odd = duet.inputType(0);
    AW_REQUIRE(!odd.hasValue());
    AW_CHECK_EQ(odd.error().code(), core::ErrorCode::ProtocolError);

    usb.attached = false;
    auto gone = duet.inputType(0);
    AW_REQUIRE(!gone.hasValue());
    AW_CHECK_EQ(gone.error().code(), core::ErrorCode::DeviceNotConnected);
}

AW_TEST(ApogeeDuet, NamesEachType) {
    AW_CHECK_EQ(devices::toString(devices::DuetInputType::Line4dBu), std::string_view{"+4DBU"});
    AW_CHECK_EQ(devices::toString(devices::DuetInputType::Instrument), std::string_view{"INST"});
}

AW_TEST(ApogeeDuet, TalksToTheOriginalDuet2Too) {
    FakeDuet usb;
    usb.productId = devices::ApogeeDuet::kOriginalProductId;
    usb.settings[{22, 1}] = 2;
    devices::ApogeeDuet duet{usb};
    auto type = duet.inputType(1);
    AW_REQUIRE(type.hasValue());
    AW_CHECK(type.value() == devices::DuetInputType::Microphone);
    AW_REQUIRE(duet.setInputType(1, devices::DuetInputType::Instrument).hasValue());
    AW_CHECK_EQ((usb.settings[{22, 1}]), std::uint8_t{3});
}
