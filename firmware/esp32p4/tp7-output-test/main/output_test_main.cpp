///
/// Bench test for the TP-7 monitor output (port step P3).
///
/// Plays a 660 Hz sine at -18 dBFS from a 48 kHz stereo render callback
/// through `Tp7AudioOutput`, exactly as the engine would, and records it on
/// the TP-7 to check it objectively. One lab cycle, about 35 s:
///
///   0 s   open and start the output as soon as a TP-7 is in USB-audio mode
///   6 s   CC 14 = 127 (arm recording), 9 s Start, 14 s Stop, 15 s CC 14 = 0
///  20 s   switch the TP-7 to MTP while streaming (the output must notice the
///         device going away), then send the newest take to the console as
///         base64 lines between LABTAKE and LABEND, with a CRC-32
///
/// tp7-audio-probe/tools/collect.py reads the console, saves and analyses the
/// takes. Each cycle prints "LAB run N" when it starts streaming;
/// tools/wait_safe.py waits for it so a flash never cuts an MTP transfer.
///
#include <algorithm>
#include <atomic>
#include <optional>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "aw/core/AudioFormat.hpp"
#include "aw/core/Logger.hpp"
#include "aw/platform/esp32p4/EspConsole.hpp"
#include "aw/platform/esp32p4/EspUsb.hpp"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/Tp7AudioOutput.hpp"

namespace {

using namespace aw;

constexpr std::string_view kModule = "lab";
constexpr std::uint16_t kTeenageEngineering = 0x2367;

/// The engine's side: 48 kHz stereo, one period per call.
class Sine final : public hal::AudioRenderCallback {
public:
    void renderAudio(std::span<float> output, core::FrameCount frames,
                     core::ChannelCount channels) noexcept override {
        for (core::FrameCount frame = 0; frame < frames; ++frame) {
            const float value = kAmplitude * static_cast<float>(std::sin(phase_));
            phase_ += kStep;
            if (phase_ > 2.0 * M_PI) phase_ -= 2.0 * M_PI;
            for (core::ChannelCount channel = 0; channel < channels; ++channel)
                output[static_cast<std::size_t>(frame) * channels + channel] = value;
        }
    }
    void onXRun() noexcept override { ++xruns; }

    std::atomic<std::uint32_t> xruns{0};

private:
    static constexpr double kStep = 2.0 * M_PI * 660.0 / 48000.0;
    static constexpr float kAmplitude = 0.12589254F;  // -18 dBFS
    double phase_{0.0};
};

void sleepMs(int ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

void line(const std::string& text) { platform::esp32p4::consoleWrite(text + "\n"); }

/// Bytes to the console as "B64 <data>" lines of 57 bytes, with a CRC-32.
struct Base64Dump {
    std::uint8_t pending[57];
    std::size_t used{0};
    std::uint32_t crc{0};
    std::size_t total{0};

    void flushLine() {
        static constexpr char kAlphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string text = "B64 ";
        for (std::size_t i = 0; i < used; i += 3) {
            const std::uint32_t b0 = pending[i];
            const std::uint32_t b1 = i + 1 < used ? pending[i + 1] : 0;
            const std::uint32_t b2 = i + 2 < used ? pending[i + 2] : 0;
            const std::uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
            text += kAlphabet[(triple >> 18) & 63];
            text += kAlphabet[(triple >> 12) & 63];
            text += i + 1 < used ? kAlphabet[(triple >> 6) & 63] : '=';
            text += i + 2 < used ? kAlphabet[triple & 63] : '=';
        }
        line(text);
        used = 0;
    }

    void add(std::span<const std::byte> bytes) {
        const auto* raw = reinterpret_cast<const std::uint8_t*>(bytes.data());
        crc = esp_rom_crc32_le(crc, raw, static_cast<std::uint32_t>(bytes.size()));
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            pending[used++] = raw[i];
            ++total;
            if (used == sizeof pending) flushLine();
        }
    }
};

std::optional<hal::UsbDeviceInfo> findTp7(hal::USBInterface& usb, bool mtp) {
    auto devices = usb.enumerate();
    if (!devices) return std::nullopt;
    for (const auto& device : devices.value())
        if (device.vendorId == kTeenageEngineering && device.supportsMtp == mtp) return device;
    return std::nullopt;
}

void logStats(const platform::esp32p4::usb::Tp7AudioOutput& output, const Sine& sine) {
    const auto s = output.stats();
    char text[256];
    std::snprintf(text, sizeof text,
                  "TP7 packets %" PRIu32 ", frames %" PRIu64 ", underruns %" PRIu32
                  ", failed %" PRIu32 ", fifo %" PRIu32 ", rate %.4f frames/ms, feedback %" PRIu32
                  " (empty %" PRIu32 "), resets %" PRIu32 ", xruns seen %" PRIu32,
                  s.packets, s.frames, s.underruns, s.failedPackets, s.fifoFrames,
                  s.framesPerMsQ16 / 65536.0, s.feedbackReports, s.emptyFeedback, s.resets,
                  sine.xruns.load());
    line(text);
}

/// Sends the newest take in recordings/ to the console.
void fetchNewestTake(hal::MTPInterface& mtp, const hal::UsbDeviceInfo& device, int run) {
    if (auto status = mtp.openSession(device.key()); !status) {
        line("LAB MTP session failed: " + status.error().describe());
        return;
    }
    auto storages = mtp.storages();
    std::optional<hal::MtpObjectInfo> take;
    if (storages && !storages.value().empty()) {
        const hal::MtpStorageId storage = storages.value().front().id;
        if (auto root = mtp.listObjects(storage, hal::kMtpRootHandle); root) {
            for (const auto& folder : root.value()) {
                if (!folder.isFolder || folder.name != "recordings") continue;
                if (auto files = mtp.listObjects(storage, folder.handle); files) {
                    // Lab takes are dated ("2026-10-17_113422_000.wav") and short;
                    // the folder also holds imported files of any size.
                    for (const auto& file : files.value()) {
                        if (file.isFolder || !file.name.starts_with("20") || file.sizeBytes > (4u << 20))
                            continue;
                        if (!take || file.modifiedUnixSeconds > take->modifiedUnixSeconds ||
                            (file.modifiedUnixSeconds == take->modifiedUnixSeconds && file.name > take->name))
                            take = file;
                    }
                    line("LAB " + std::to_string(files.value().size()) + " recordings; newest " +
                         (take ? take->name : std::string{"none"}));
                }
            }
        }
    }
    if (take) {
        Base64Dump dump;
        line("\nLABTAKE " + std::to_string(run) + " 0 " + std::to_string(take->sizeBytes) +
             " recordings/" + take->name);
        const auto status = mtp.readObject(
            take->handle,
            [&](std::span<const std::byte> chunk) {
                dump.add(chunk);
                return true;
            },
            {});
        if (dump.used > 0) dump.flushLine();
        char end[64];
        std::snprintf(end, sizeof end, "LABEND %s %u %08" PRIx32, status ? "ok" : "failed",
                      static_cast<unsigned>(dump.total), dump.crc);
        line(end);
    } else {
        line("LAB no take found");
    }
    mtp.closeSession();  // The TP-7 returns to USB audio.
}

}  // namespace

extern "C" void app_main(void) {
    platform::esp32p4::lockIdfLogging();
    core::log::installSink(std::make_shared<platform::esp32p4::EspConsoleLogSink>());
    core::log::setLevel(core::LogLevel::Info);

    platform::esp32p4::EspUsbBackends usb = platform::esp32p4::createUsbBackends();
    if (auto status = usb.usb->start(); !status) {
        core::log::error(kModule, "USB host: " + status.error().describe());
        return;
    }
    auto& output = static_cast<platform::esp32p4::usb::Tp7AudioOutput&>(*usb.audioOutput);
    Sine sine;
    line("LAB waiting for a TP-7 in USB-audio mode");

    for (int run = 0;; ++run) {
        // A TP-7 that stays in MTP mode (a cut-off earlier run) is sent back.
        // Short visits are the output's own feedback reset; leave those be.
        for (int mtpPolls = 0;;) {
            if (auto stuck = findTp7(*usb.usb, true)) {
                if (++mtpPolls < 40) {
                    sleepMs(250);
                    continue;
                }
                line("LAB TP-7 stays in MTP mode; closing a session to return it to audio");
                if (usb.mtp->openSession(stuck->key())) usb.mtp->closeSession();
                mtpPolls = 0;
                sleepMs(3000);
                continue;
            }
            if (output.open(core::kNativeFormat, 256, sine)) break;
            sleepMs(250);
        }
        if (auto status = output.start(); !status) {
            line("LAB start failed: " + status.error().describe());
            output.close();
            sleepMs(1000);
            continue;
        }
        line("LAB run " + std::to_string(run) + ": variant 0, 660 Hz sine through Tp7AudioOutput");

        struct Step {
            int ms;
            std::uint8_t packet[4];
        };
        constexpr Step kSteps[] = {{6000, {0x0B, 0xB0, 14, 127}},
                                   {9000, {0x0F, 0xFA, 0, 0}},
                                   {14000, {0x0F, 0xFC, 0, 0}},
                                   {15000, {0x0B, 0xB0, 14, 0}}};
        const std::int64_t started = esp_timer_get_time();
        std::size_t next = 0;
        std::int64_t lastStats = started;
        bool lost = false;
        while (!lost) {
            const int elapsed = static_cast<int>((esp_timer_get_time() - started) / 1000);
            if (next < std::size(kSteps) && elapsed >= kSteps[next].ms) {
                const auto status = usb.sendMidi(kSteps[next].packet);
                char text[64];
                std::snprintf(text, sizeof text, "LAB MIDI %02x %02x %02x: %s", kSteps[next].packet[1],
                              kSteps[next].packet[2], kSteps[next].packet[3], status ? "sent" : "FAILED");
                line(text);
                ++next;
            }
            if (esp_timer_get_time() - lastStats >= 1'000'000) {
                lastStats = esp_timer_get_time();
                (void)output.refresh();
                logStats(output, sine);
            }
            if (elapsed >= 20000) break;
            if (!output.isRunning()) {
                line("LAB output stopped (the TP-7 left); reopening");
                lost = true;
            }
            sleepMs(10);
        }

        if (!lost) {
            if (auto status = usb.deviceMode->requestMtpMode(); !status)
                line("LAB MTP request failed: " + status.error().describe());
            const std::int64_t asked = esp_timer_get_time();
            while (output.isRunning() && esp_timer_get_time() - asked < 5'000'000) sleepMs(10);
            line(std::string{"LAB output "} + (output.isRunning() ? "still running after 5 s"
                                                                  : "saw the TP-7 leave") +
                 " (" + std::to_string((esp_timer_get_time() - asked) / 1000) + " ms)");
        }
        logStats(output, sine);
        output.close();
        if (lost) {
            --run;
            continue;
        }

        std::optional<hal::UsbDeviceInfo> mtpDevice;
        for (int i = 0; i < 100 && !mtpDevice; ++i) {
            mtpDevice = findTp7(*usb.usb, true);
            if (!mtpDevice) sleepMs(100);
        }
        if (mtpDevice) {
            line("LAB TP-7 is in MTP mode; fetching the newest take");
            fetchNewestTake(*usb.mtp, *mtpDevice, run);
        } else {
            line("LAB the TP-7 did not come back in MTP mode");
        }
        sleepMs(1000);
    }
}
