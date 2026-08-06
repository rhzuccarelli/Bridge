#include "aw/platform/linux/SpiOledBus.hpp"

#include <fcntl.h>
#include <linux/gpio.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <thread>

namespace aw::platform::linuxfb {
namespace {

// Position of each line within the GPIO request's offsets array.
constexpr unsigned int kDcIndex = 0;
constexpr unsigned int kResIndex = 1;

core::Error errnoError(const char* what) {
    return core::makeError(core::ErrorCode::IoError,
                           std::string{what} + ": " + std::strerror(errno));
}

}  // namespace

SpiOledBus::SpiOledBus(SpiOledConfig config) : config_{std::move(config)} {}

SpiOledBus::~SpiOledBus() {
    if (spiFd_ >= 0) {
        ::close(spiFd_);
    }
    if (lineFd_ >= 0) {
        ::close(lineFd_);
    }
}

bool SpiOledBus::isAvailable(const std::string& spiDevice) noexcept {
    return ::access(spiDevice.c_str(), F_OK) == 0;
}

core::Status SpiOledBus::openBus() {
    // --- SPI ----------------------------------------------------------------
    spiFd_ = ::open(config_.spiDevice.c_str(), O_RDWR);
    if (spiFd_ < 0) {
        return core::makeError(core::ErrorCode::NotFound,
                               config_.spiDevice + " (is 'dtparam=spi=on' set?): " +
                                   std::strerror(errno));
    }

    std::uint8_t mode = SPI_MODE_0;
    std::uint8_t bits = 8;
    if (ioctl(spiFd_, SPI_IOC_WR_MODE, &mode) < 0 ||
        ioctl(spiFd_, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(spiFd_, SPI_IOC_WR_MAX_SPEED_HZ, &config_.speedHz) < 0) {
        return errnoError("configuring spidev");
    }

    // --- GPIO (D/C and RES as outputs, via the character device) ------------
    const int chipFd = ::open(config_.gpioChip.c_str(), O_RDONLY | O_CLOEXEC);
    if (chipFd < 0) {
        return core::makeError(core::ErrorCode::NotFound,
                               config_.gpioChip + ": " + std::strerror(errno));
    }

    gpio_v2_line_request request{};
    request.offsets[kDcIndex] = config_.dataCommandLine;
    request.offsets[kResIndex] = config_.resetLine;
    request.num_lines = 2;
    request.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
    std::strncpy(request.consumer, "aw-oled", sizeof(request.consumer) - 1);

    const int requestResult = ioctl(chipFd, GPIO_V2_GET_LINE_IOCTL, &request);
    ::close(chipFd);  // The request fd outlives the chip fd; the chip fd is done.
    if (requestResult < 0 || request.fd < 0) {
        return errnoError("requesting GPIO lines");
    }
    lineFd_ = request.fd;

    // Idle state: D/C low, reset de-asserted (high).
    gpio_v2_line_values values{};
    values.mask = (1ULL << kDcIndex) | (1ULL << kResIndex);
    values.bits = (1ULL << kResIndex);
    if (ioctl(lineFd_, GPIO_V2_LINE_SET_VALUES_IOCTL, &values) < 0) {
        return errnoError("setting initial GPIO values");
    }
    return core::success();
}

core::Status SpiOledBus::setDataCommand(bool data) {
    gpio_v2_line_values values{};
    values.mask = (1ULL << kDcIndex);
    values.bits = data ? (1ULL << kDcIndex) : 0;
    if (ioctl(lineFd_, GPIO_V2_LINE_SET_VALUES_IOCTL, &values) < 0) {
        return errnoError("toggling D/C");
    }
    return core::success();
}

core::Status SpiOledBus::writeBytes(std::span<const std::uint8_t> bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const ssize_t n = ::write(spiFd_, bytes.data() + sent, bytes.size() - sent);
        if (n <= 0) {
            return errnoError("SPI write");
        }
        sent += static_cast<std::size_t>(n);
    }
    return core::success();
}

core::Status SpiOledBus::reset() {
    if (lineFd_ < 0) {
        return core::makeError(core::ErrorCode::InvalidState, "bus not open");
    }
    using namespace std::chrono_literals;

    const auto driveReset = [this](bool high) -> core::Status {
        gpio_v2_line_values values{};
        values.mask = (1ULL << kResIndex);
        values.bits = high ? (1ULL << kResIndex) : 0;
        if (ioctl(lineFd_, GPIO_V2_LINE_SET_VALUES_IOCTL, &values) < 0) {
            return errnoError("driving RES");
        }
        return core::success();
    };

    if (auto status = driveReset(true); !status) return status;
    std::this_thread::sleep_for(1ms);
    if (auto status = driveReset(false); !status) return status;  // Pulse low.
    std::this_thread::sleep_for(10ms);
    if (auto status = driveReset(true); !status) return status;   // Release.
    std::this_thread::sleep_for(10ms);
    return core::success();
}

core::Status SpiOledBus::writeCommands(std::span<const std::uint8_t> commands) {
    if (auto status = setDataCommand(false); !status) {
        return status;
    }
    return writeBytes(commands);
}

core::Status SpiOledBus::writeData(std::span<const std::uint8_t> data) {
    if (auto status = setDataCommand(true); !status) {
        return status;
    }
    return writeBytes(data);
}

}  // namespace aw::platform::linuxfb
