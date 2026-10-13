#include "Tp7Midi.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace probe {
namespace {

// Captured from Teenage Engineering Field Kit 2.0.1 while switching a TP-7
// running OS 1.1.11 from its USB audio/MIDI personality into MTP.
constexpr std::array<std::uint8_t, 13> kEnterMtp{
    0xF0, 0x00, 0x20, 0x76, 0x19, 0x40, 0x60,
    0x3C, 0x04, 0x00, 0x01, 0x03, 0xF7,
};

/// USB-MIDI 1.0 section 4: SysEx travels in 4-byte event packets on cable 0.
/// CIN 0x4 carries three bytes of a continuing message; CIN 0x5/0x6/0x7 end it
/// with one, two or three bytes.
std::vector<std::uint8_t> packSysEx(const std::uint8_t* bytes, std::size_t size) {
    std::vector<std::uint8_t> packets;
    std::size_t offset = 0;
    while (offset < size) {
        const std::size_t left = size - offset;
        const std::size_t take = left > 3 ? 3 : left;
        const bool last = take == left;
        const std::uint8_t cin = !last ? 0x4 : static_cast<std::uint8_t>(0x4 + take);
        packets.push_back(cin);
        for (std::size_t i = 0; i < 3; ++i)
            packets.push_back(i < take ? bytes[offset + i] : 0x00);
        offset += take;
    }
    return packets;
}

}  // namespace

esp_err_t sendTp7EnterMtp(BulkPipe& midiOut) {
    const auto packets = packSysEx(kEnterMtp.data(), kEnterMtp.size());
    return midiOut.write(packets, false, 1000);
}

}  // namespace probe
