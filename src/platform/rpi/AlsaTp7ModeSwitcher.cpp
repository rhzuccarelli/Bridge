#include "aw/platform/rpi/AlsaTp7ModeSwitcher.hpp"

#include <alsa/asoundlib.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>

namespace aw::platform::rpi {
namespace {

// Captured from Teenage Engineering Field Kit 2.0.1 while switching a TP-7
// running OS 1.1.11 from its USB audio/MIDI personality into MTP.
constexpr std::array<std::uint8_t, 13> kEnterMtp{
    0xF0, 0x00, 0x20, 0x76, 0x19, 0x40, 0x60,
    0x3C, 0x04, 0x00, 0x01, 0x03, 0xF7,
};

core::Error alsaError(int code, const char* operation) {
    return core::makeError(code == -ENOENT || code == -ENODEV
                               ? core::ErrorCode::DeviceNotConnected
                               : core::ErrorCode::IoError,
                           std::string{operation} + ": " + snd_strerror(code));
}

}  // namespace

core::Status AlsaTp7ModeSwitcher::requestMtpMode() {
    snd_rawmidi_t* output = nullptr;
    const int opened = snd_rawmidi_open(nullptr, &output, device_.c_str(), 0);
    if (opened < 0) return alsaError(opened, "open TP-7 MIDI output");

    std::size_t sent = 0;
    while (sent < kEnterMtp.size()) {
        const auto written = snd_rawmidi_write(output, kEnterMtp.data() + sent,
                                               kEnterMtp.size() - sent);
        if (written < 0) {
            snd_rawmidi_close(output);
            return alsaError(written, "send TP-7 MTP request");
        }
        if (written == 0) {
            snd_rawmidi_close(output);
            return core::makeError(core::ErrorCode::IoError,
                                   "send TP-7 MTP request: no MIDI bytes written");
        }
        sent += static_cast<std::size_t>(written);
    }
    const int drained = snd_rawmidi_drain(output);
    snd_rawmidi_close(output);
    if (drained < 0) return alsaError(drained, "finish TP-7 MTP request");
    return core::success();
}

}  // namespace aw::platform::rpi
