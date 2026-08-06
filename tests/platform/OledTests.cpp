///
/// Tests for the SSD1306/SH1106 controller logic.
///
/// The valuable, get-it-right-once part of an OLED driver is the framebuffer
/// packing and the command stream, and both are pure logic behind the `OledBus`
/// seam. A fake bus records every byte, so these run with no SPI, no GPIO and no
/// panel - which is the whole reason the transport was made injectable.
///
#include "aw/display/Color.hpp"
#include "aw/platform/oled/Ssd1306Display.hpp"
#include "framework/Test.hpp"

#include <array>
#include <vector>

using namespace aw;
using namespace aw::platform::oled;

namespace {

/// Records the command and data streams the display would send.
class FakeOledBus final : public OledBus {
public:
    core::Status reset() override {
        resetCount++;
        return core::success();
    }
    core::Status writeCommands(std::span<const std::uint8_t> bytes) override {
        commands.insert(commands.end(), bytes.begin(), bytes.end());
        return core::success();
    }
    core::Status writeData(std::span<const std::uint8_t> bytes) override {
        data.insert(data.end(), bytes.begin(), bytes.end());
        return core::success();
    }

    int resetCount{0};
    std::vector<std::uint8_t> commands;
    std::vector<std::uint8_t> data;
};

/// One fully-lit and one black RGB565 pixel.
constexpr std::uint16_t kWhite = 0xFFFF;
constexpr std::uint16_t kBlack = 0x0000;

std::unique_ptr<Ssd1306Display> makeDisplay(FakeOledBus** busOut,
                                            Controller controller = Controller::Ssd1306) {
    auto bus = std::make_unique<FakeOledBus>();
    *busOut = bus.get();
    OledConfig config;
    config.controller = controller;
    return std::make_unique<Ssd1306Display>(config, std::move(bus));
}

}  // namespace

AW_TEST(Oled, ReportsItsPanelSize) {
    FakeOledBus* bus = nullptr;
    auto display = makeDisplay(&bus);
    const hal::DisplayInfo info = display->info();
    AW_CHECK_EQ(info.width, std::uint16_t{128});
    AW_CHECK_EQ(info.height, std::uint16_t{64});
}

AW_TEST(Oled, ThresholdsColourToOneBit) {
    AW_CHECK(Ssd1306Display::isLit(kWhite));
    AW_CHECK(!Ssd1306Display::isLit(kBlack));
    AW_CHECK(!Ssd1306Display::isLit(aw::display::palette::kBackground));  // near-black bg
    AW_CHECK(Ssd1306Display::isLit(aw::display::palette::kText));         // bright text
    AW_CHECK(Ssd1306Display::isLit(aw::display::palette::kAccent));       // amber accent
}

AW_TEST(Oled, PacksPixelsIntoTheCorrectPageBit) {
    FakeOledBus* bus = nullptr;
    auto display = makeDisplay(&bus);

    // (0,0) is page 0, bit 0 -> buffer[0] bit 0.
    display->setPixel(0, 0, true);
    AW_CHECK(display->pixel(0, 0));
    AW_CHECK_EQ(display->pageBuffer()[0], std::uint8_t{0x01});

    // (0,7) is the bottom of page 0 -> buffer[0] bit 7.
    display->setPixel(0, 7, true);
    AW_CHECK_EQ(display->pageBuffer()[0], std::uint8_t{0x81});

    // (0,8) drops into page 1 -> buffer[128] bit 0.
    display->setPixel(0, 8, true);
    AW_CHECK(display->pixel(0, 8));
    AW_CHECK_EQ(display->pageBuffer()[128], std::uint8_t{0x01});

    // (5,0) -> buffer[5] bit 0, independent of the others.
    display->setPixel(5, 0, true);
    AW_CHECK_EQ(display->pageBuffer()[5], std::uint8_t{0x01});
}

AW_TEST(Oled, ClearingABitLeavesNeighboursAlone) {
    FakeOledBus* bus = nullptr;
    auto display = makeDisplay(&bus);

    display->setPixel(0, 0, true);
    display->setPixel(0, 1, true);
    display->setPixel(0, 0, false);

    AW_CHECK(!display->pixel(0, 0));
    AW_CHECK(display->pixel(0, 1));
    AW_CHECK_EQ(display->pageBuffer()[0], std::uint8_t{0x02});
}

AW_TEST(Oled, IgnoresOutOfRangePixels) {
    FakeOledBus* bus = nullptr;
    auto display = makeDisplay(&bus);

    display->setPixel(-1, 0, true);
    display->setPixel(0, -1, true);
    display->setPixel(128, 0, true);
    display->setPixel(0, 64, true);

    AW_CHECK(!display->pixel(-1, 0));
    AW_CHECK(!display->pixel(128, 0));
    // Nothing was written anywhere.
    for (auto byte : display->pageBuffer()) {
        AW_CHECK_EQ(byte, std::uint8_t{0});
    }
}

AW_TEST(Oled, BlitThresholdsAndPlacesPixels) {
    FakeOledBus* bus = nullptr;
    auto display = makeDisplay(&bus);

    // A 2x2 block at (10,10): three white, one black.
    const std::array<std::uint16_t, 4> pixels{kWhite, kBlack, kWhite, kWhite};
    AW_REQUIRE(static_cast<bool>(display->blit(hal::Rect{10, 10, 2, 2}, pixels)));

    AW_CHECK(display->pixel(10, 10));   // white
    AW_CHECK(!display->pixel(11, 10));  // black
    AW_CHECK(display->pixel(10, 11));   // white
    AW_CHECK(display->pixel(11, 11));   // white
}

AW_TEST(Oled, OpenResetsInitialisesAndPushesACleanFrame) {
    FakeOledBus* bus = nullptr;
    auto display = makeDisplay(&bus);

    AW_REQUIRE(static_cast<bool>(display->open()));

    AW_CHECK_EQ(bus->resetCount, 1);
    AW_CHECK(!bus->commands.empty());
    // First init byte is display-off (0xAE); last is display-on (0xAF).
    AW_CHECK_EQ(bus->commands.front(), std::uint8_t{0xAE});
    // Charge pump on (0x8D 0x14) is present for the SSD1306.
    bool chargePump = false;
    for (std::size_t i = 0; i + 1 < bus->commands.size(); ++i) {
        if (bus->commands[i] == 0x8D && bus->commands[i + 1] == 0x14) {
            chargePump = true;
        }
    }
    AW_CHECK(chargePump);

    // A blank 128x64 frame is 8 pages x 128 bytes = 1024 data bytes, all zero.
    AW_CHECK_EQ(bus->data.size(), std::size_t{1024});
    for (auto byte : bus->data) {
        AW_CHECK_EQ(byte, std::uint8_t{0});
    }
}

AW_TEST(Oled, FlushEmitsPageAddressingForEveryPage) {
    FakeOledBus* bus = nullptr;
    auto display = makeDisplay(&bus);
    AW_REQUIRE(static_cast<bool>(display->open()));

    // Count page-start commands (0xB0..0xB7) in the stream.
    int pageStarts = 0;
    for (auto byte : bus->commands) {
        if (byte >= 0xB0 && byte <= 0xB7) {
            ++pageStarts;
        }
    }
    AW_CHECK_EQ(pageStarts, 8);  // One per page of a 64px-tall panel.
}

AW_TEST(Oled, Sh1106UsesADifferentChargePumpAndColumnOffset) {
    FakeOledBus* bus = nullptr;
    auto display = makeDisplay(&bus, Controller::Sh1106);
    AW_REQUIRE(static_cast<bool>(display->open()));

    // SH1106 charge pump is 0xAD 0x8B, not 0x8D 0x14.
    bool sh1106Pump = false;
    bool ssdPump = false;
    for (std::size_t i = 0; i + 1 < bus->commands.size(); ++i) {
        if (bus->commands[i] == 0xAD && bus->commands[i + 1] == 0x8B) sh1106Pump = true;
        if (bus->commands[i] == 0x8D && bus->commands[i + 1] == 0x14) ssdPump = true;
    }
    AW_CHECK(sh1106Pump);
    AW_CHECK(!ssdPump);

    // The 2-column offset appears as a low-column-start command of 0x02.
    bool hasOffset = false;
    for (auto byte : bus->commands) {
        if (byte == 0x02) hasOffset = true;  // 0x00 | (2 & 0x0F)
    }
    AW_CHECK(hasOffset);
}

AW_TEST(Oled, PresentIsANoOpWhenNothingChanged) {
    FakeOledBus* bus = nullptr;
    auto display = makeDisplay(&bus);
    AW_REQUIRE(static_cast<bool>(display->open()));

    const std::size_t dataAfterOpen = bus->data.size();
    AW_REQUIRE(static_cast<bool>(display->present()));  // No blit since open.
    AW_CHECK_EQ(bus->data.size(), dataAfterOpen);       // Nothing re-sent.

    display->setPixel(0, 0, true);
    // setPixel marks dirty; present should now push a fresh 1024-byte frame.
    AW_REQUIRE(static_cast<bool>(display->present()));
    AW_CHECK_EQ(bus->data.size(), dataAfterOpen + 1024);
}

AW_TEST(Oled, TestPatternLightsTheBorder) {
    FakeOledBus* bus = nullptr;
    auto display = makeDisplay(&bus);
    AW_REQUIRE(static_cast<bool>(display->open()));
    AW_REQUIRE(static_cast<bool>(display->showTestPattern()));

    // All four corners are on the border, so all lit.
    AW_CHECK(display->pixel(0, 0));
    AW_CHECK(display->pixel(127, 0));
    AW_CHECK(display->pixel(0, 63));
    AW_CHECK(display->pixel(127, 63));
    // The centre is not on the border or the diagonal's ends; it should be off
    // unless the diagonal crosses it. Pick a point clearly off both.
    AW_CHECK(!display->pixel(64, 5));
}
