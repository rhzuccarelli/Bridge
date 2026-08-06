///
/// OLED bring-up tool.
///
/// Draws a border + diagonal + origin block on the panel and holds it, so the
/// wiring, the controller choice and the orientation can be confirmed before
/// the UI (or even the font) exists. This is the first thing to run after
/// wiring the display.
///
///     aw-oled-test                 # SSD1306 128x64 on /dev/spidev0.0
///     aw-oled-test sh1106          # if the image is shifted/split, it is an SH1106
///     AW_OLED_ROTATE=180 aw-oled-test
///
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "aw/platform/linux/SpiOledBus.hpp"
#include "aw/platform/oled/Ssd1306Display.hpp"

namespace {
volatile std::sig_atomic_t g_stop = 0;
extern "C" void onSignal(int) { g_stop = 1; }
}  // namespace

int main(int argc, char** argv) {
    using namespace aw;

    const std::string spi = std::getenv("AW_OLED_SPI") ? std::getenv("AW_OLED_SPI")
                                                        : "/dev/spidev0.0";
    const bool sh1106 = argc > 1 && std::strcmp(argv[1], "sh1106") == 0;
    const bool upsideDown =
        std::getenv("AW_OLED_ROTATE") && std::strcmp(std::getenv("AW_OLED_ROTATE"), "180") == 0;

    auto bus = std::make_unique<platform::linuxfb::SpiOledBus>(
        platform::linuxfb::SpiOledConfig{spi});
    if (auto status = bus->openBus(); !status) {
        std::fprintf(stderr, "OLED bus failed: %s\n", status.error().describe().c_str());
        std::fprintf(stderr, "  - is SPI enabled?  add 'dtparam=spi=on' and reboot\n");
        std::fprintf(stderr, "  - are you in the 'spi' and 'gpio' groups?\n");
        return EXIT_FAILURE;
    }

    platform::oled::OledConfig config;
    config.controller =
        sh1106 ? platform::oled::Controller::Sh1106 : platform::oled::Controller::Ssd1306;
    config.rotation =
        upsideDown ? platform::oled::Rotation::UpsideDown : platform::oled::Rotation::Normal;

    platform::oled::Ssd1306Display display{config, std::move(bus)};

    if (auto status = display.open(); !status) {
        std::fprintf(stderr, "OLED init failed: %s\n", status.error().describe().c_str());
        return EXIT_FAILURE;
    }
    if (auto status = display.showTestPattern(); !status) {
        std::fprintf(stderr, "test pattern failed: %s\n", status.error().describe().c_str());
        return EXIT_FAILURE;
    }

    std::printf("Test pattern shown on %s (%s).\n", spi.c_str(), sh1106 ? "SH1106" : "SSD1306");
    std::printf("You should see: a 1px border, a diagonal, and a filled block top-left.\n");
    std::printf("  - nothing at all      -> check power, wiring, and the SPI/GPIO groups\n");
    std::printf("  - shifted or split    -> wrong controller; try 'aw-oled-test sh1106'\n");
    std::printf("  - upside down/mirrored -> run with AW_OLED_ROTATE=180\n");
    std::printf("Ctrl-C to exit.\n");

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    while (g_stop == 0) {
        pause();
    }

    display.close();
    return EXIT_SUCCESS;
}
