#include "aw/platform/rpi/AlsaAudioOutput.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numbers>
#include <span>
#include <string>
#include <thread>

namespace {

class StereoTone final : public aw::hal::AudioRenderCallback {
public:
    void renderAudio(std::span<float> output,
                     aw::core::FrameCount frames,
                     aw::core::ChannelCount channels) noexcept override {
        constexpr double kTau = std::numbers::pi * 2.0;
        for (aw::core::FrameCount frame = 0; frame < frames; ++frame) {
            const std::size_t offset = static_cast<std::size_t>(frame) * channels;
            output[offset] = static_cast<float>(std::sin(leftPhase_) * 0.2);
            if (channels > 1) {
                output[offset + 1] = static_cast<float>(std::sin(rightPhase_) * 0.2);
            }
            leftPhase_ += kTau * 440.0 / 48000.0;
            rightPhase_ += kTau * 660.0 / 48000.0;
            if (leftPhase_ >= kTau) {
                leftPhase_ -= kTau;
            }
            if (rightPhase_ >= kTau) {
                rightPhase_ -= kTau;
            }
        }
    }

    void onXRun() noexcept override { xruns_.fetch_add(1, std::memory_order_relaxed); }
    [[nodiscard]] std::uint32_t xruns() const noexcept {
        return xruns_.load(std::memory_order_relaxed);
    }

private:
    double leftPhase_{0.0};
    double rightPhase_{0.0};
    std::atomic<std::uint32_t> xruns_{0};
};

}  // namespace

int main() {
    const char* configured = std::getenv("AW_ALSA_DEVICE");
    const std::string device = configured != nullptr ? configured : "hw:0,0";
    aw::platform::rpi::AlsaAudioOutput output{device};
    StereoTone tone;

    if (auto status = output.open(aw::core::kNativeFormat, 256, tone); !status) {
        std::cerr << "open failed: " << status.error().describe() << '\n';
        return EXIT_FAILURE;
    }
    if (auto status = output.start(); !status) {
        std::cerr << "start failed: " << status.error().describe() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "Playing 440 Hz left / 660 Hz right for 5 seconds on " << device << '\n';
    std::this_thread::sleep_for(std::chrono::seconds{5});
    output.stop();
    std::cout << "period=" << output.bufferFrames()
              << " frames, buffer=" << output.latencyFrames()
              << " frames, xruns=" << tone.xruns() << '\n';
    return tone.xruns() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
