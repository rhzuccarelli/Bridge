#include "aw/platform/rpi/GpioInput.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {

class Reporter final : public aw::hal::ButtonListener, public aw::hal::EncoderListener {
public:
    void onButtonEvent(aw::core::ButtonId id,
                       aw::core::ButtonAction action,
                       std::uint64_t) noexcept override {
        std::cout << "button " << aw::core::toString(id) << ' '
                  << aw::core::toString(action) << std::endl;
    }

    void onEncoderTurned(std::int32_t delta, std::uint64_t) noexcept override {
        std::cout << "encoder " << (delta > 0 ? "+1" : "-1") << std::endl;
    }
};

}  // namespace

int main() {
    aw::platform::rpi::GpioPinMap pins = aw::platform::rpi::GpioPinMap::prototype();
    if (const char* chip = std::getenv("AW_GPIO_CHIP")) pins.chipName = chip;

    Reporter reporter;
    aw::platform::rpi::GpioButtons buttons{pins};
    aw::platform::rpi::GpioEncoder encoder{pins};
    buttons.setListener(&reporter);
    encoder.setListener(&reporter);

    if (auto status = buttons.start(); !status) {
        std::cerr << "buttons failed: " << status.error().describe() << '\n';
        return EXIT_FAILURE;
    }
    if (auto status = encoder.start(); !status) {
        std::cerr << "encoder failed: " << status.error().describe() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "GPIO test running for 30 seconds on " << pins.chipName << "\n"
              << "Turn the encoder both ways, click it, and hold it for one second.\n";
    std::this_thread::sleep_for(std::chrono::seconds{30});
    encoder.stop();
    buttons.stop();
    return EXIT_SUCCESS;
}
