#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

namespace aw::platform {

/// Playing the engine's 48 kHz stereo through a TP-7's USB playback endpoint.
/// Pure logic, shared by the Raspberry Pi (ALSA) and ESP32-P4 (USB host)
/// backends and tested on the host.

/// Converts one normalized float sample to packed signed 24-bit little-endian,
/// the TP-7's (and ALSA's S24_3LE) sample container.
inline void packS24_3LE(float value, std::span<std::byte, 3> destination) noexcept {
    const float clamped = std::clamp(value, -1.0F, 1.0F);
    const auto sample = static_cast<std::int32_t>(
        std::lrint(clamped >= 1.0F ? 8388607.0F : clamped * 8388608.0F));
    destination[0] = static_cast<std::byte>(sample & 0xFF);
    destination[1] = static_cast<std::byte>((sample >> 8) & 0xFF);
    destination[2] = static_cast<std::byte>((sample >> 16) & 0xFF);
}

inline constexpr std::size_t kTp7MonitorChannels = 6;
inline constexpr std::uint32_t kTp7MonitorRate = 96000;
/// One 96 kHz frame on the wire: six channels of 24-bit in 3 bytes.
inline constexpr std::size_t kTp7FrameBytes = kTp7MonitorChannels * 3;

struct Tp7MonitorConverterState {
    float previousLeft{0.0F};
    float previousRight{0.0F};
};

/// Converts one 48 kHz stereo block to the TP-7's native 96 kHz, six-channel
/// USB playback layout. FL/FR carry the monitor mix; all other channels are
/// silent. Linear interpolation avoids the strong imaging of sample repetition.
inline void convertStereo48ToTp7Monitor96(std::span<const float> stereo,
                                          std::span<float> tp7,
                                          Tp7MonitorConverterState& state) noexcept {
    const std::size_t inputFrames = stereo.size() / 2U;
    const std::size_t outputFrames = std::min(tp7.size() / kTp7MonitorChannels,
                                               inputFrames * 2U);
    std::fill(tp7.begin(), tp7.end(), 0.0F);
    for (std::size_t frame = 0; frame < inputFrames && frame * 2U < outputFrames; ++frame) {
        const float left = stereo[frame * 2U];
        const float right = stereo[frame * 2U + 1U];
        const std::size_t first = frame * 2U * kTp7MonitorChannels;
        tp7[first] = state.previousLeft;
        tp7[first + 1U] = state.previousRight;
        if (frame * 2U + 1U < outputFrames) {
            const std::size_t second = first + kTp7MonitorChannels;
            tp7[second] = (state.previousLeft + left) * 0.5F;
            tp7[second + 1U] = (state.previousRight + right) * 0.5F;
        }
        state.previousLeft = left;
        state.previousRight = right;
    }
}

/// Reads a UAC1 asynchronous feedback value as frames per millisecond, in
/// Q16.16. Devices disagree on the encoding, so this does what Linux's
/// snd-usb-audio does: try each one and keep the first that lands within 5 %
/// of nominal. The TP-7 sends Q10.14 per 1 ms frame even at high speed.
/// Nullopt when no reading is plausible; keep the previous rate then.
[[nodiscard]] inline std::optional<std::uint32_t> decodeUac1Feedback(
    std::uint32_t raw, std::uint32_t nominalRate) noexcept {
    const double nominal = nominalRate / 1000.0;
    const double candidates[] = {
        raw / 65536.0 * 8.0,  // Q16.16 per microframe
        raw / 65536.0,        // Q16.16 per frame
        raw / 16384.0,        // Q10.14 per frame
    };
    for (const double framesPerMs : candidates) {
        if (std::fabs(framesPerMs - nominal) < nominal * 0.05) {
            return static_cast<std::uint32_t>(std::lround(framesPerMs * 65536.0));
        }
    }
    return std::nullopt;
}

/// Frames to put in each 1 ms isochronous packet so the stream runs at the
/// rate the device asks for. The fractional part carries over, so 96.5
/// frames/ms becomes alternating packets of 96 and 97.
class IsoPacketSizer {
public:
    explicit IsoPacketSizer(std::uint32_t nominalRate) noexcept
        : rateQ16_{static_cast<std::uint32_t>((std::uint64_t{nominalRate} << 16) / 1000)} {}

    /// From the feedback endpoint's task; read on the next packet.
    void setRateQ16(std::uint32_t framesPerMsQ16) noexcept {
        rateQ16_.store(framesPerMsQ16, std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint32_t rateQ16() const noexcept {
        return rateQ16_.load(std::memory_order_relaxed);
    }

    /// The next packet's frame count, between 1 and `maxFrames`.
    [[nodiscard]] std::uint32_t next(std::uint32_t maxFrames) noexcept {
        const std::uint32_t due = remainderQ16_ + rateQ16_.load(std::memory_order_relaxed);
        remainderQ16_ = due & 0xFFFF;
        return std::clamp<std::uint32_t>(due >> 16, 1, maxFrames);
    }

    void reset() noexcept { remainderQ16_ = 0; }

private:
    std::atomic<std::uint32_t> rateQ16_;
    std::uint32_t remainderQ16_{0};
};

/// Single-producer, single-consumer FIFO of fixed-size frames, moved in bulk.
/// The render task writes whole periods; the USB completion callback reads
/// one packet's worth. Lock-free and allocation-free after construction, so
/// it is safe on either side of the realtime boundary.
///
/// `core::RingBuffer` moves one item at a time; here a packet is up to 98
/// frames, so this copies with at most two `memcpy`s per call instead.
class FrameFifo {
public:
    /// `capacityFrames` is rounded up to a power of two.
    FrameFifo(std::size_t frameBytes, std::size_t capacityFrames)
        : frameBytes_{frameBytes}, capacity_{std::bit_ceil(std::max<std::size_t>(capacityFrames, 1))} {
        storage_.resize(capacity_ * frameBytes_);
    }

    FrameFifo(const FrameFifo&) = delete;
    FrameFifo& operator=(const FrameFifo&) = delete;

    [[nodiscard]] std::size_t frameBytes() const noexcept { return frameBytes_; }
    [[nodiscard]] std::size_t capacityFrames() const noexcept { return capacity_; }

    [[nodiscard]] std::size_t availableFrames() const noexcept {
        return written_.load(std::memory_order_acquire) - read_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::size_t freeFrames() const noexcept { return capacity_ - availableFrames(); }

    /// Producer side. Copies as many whole frames of `frames` as fit and
    /// returns how many.
    std::size_t write(std::span<const std::uint8_t> frames) noexcept {
        const std::size_t written = written_.load(std::memory_order_relaxed);
        const std::size_t room = capacity_ - (written - read_.load(std::memory_order_acquire));
        const std::size_t count = std::min(room, frames.size() / frameBytes_);
        copyIn(written, frames.data(), count);
        written_.store(written + count, std::memory_order_release);
        return count;
    }

    /// Consumer side. Copies up to `maxFrames` frames into `destination` and
    /// returns how many; fewer means the producer fell behind.
    std::size_t read(std::uint8_t* destination, std::size_t maxFrames) noexcept {
        const std::size_t read = read_.load(std::memory_order_relaxed);
        const std::size_t ready = written_.load(std::memory_order_acquire) - read;
        const std::size_t count = std::min(ready, maxFrames);
        copyOut(read, destination, count);
        read_.store(read + count, std::memory_order_release);
        return count;
    }

    /// Empties the FIFO. Only while neither side is running.
    void reset() noexcept {
        written_.store(0, std::memory_order_relaxed);
        read_.store(0, std::memory_order_relaxed);
    }

private:
    // Counters run freely and wrap; with a power-of-two capacity, the masked
    // index stays continuous across the wrap.
    void copyIn(std::size_t position, const std::uint8_t* source, std::size_t count) noexcept {
        const std::size_t start = position & (capacity_ - 1);
        const std::size_t first = std::min(count, capacity_ - start);
        std::memcpy(storage_.data() + start * frameBytes_, source, first * frameBytes_);
        std::memcpy(storage_.data(), source + first * frameBytes_, (count - first) * frameBytes_);
    }

    void copyOut(std::size_t position, std::uint8_t* destination, std::size_t count) const noexcept {
        const std::size_t start = position & (capacity_ - 1);
        const std::size_t first = std::min(count, capacity_ - start);
        std::memcpy(destination, storage_.data() + start * frameBytes_, first * frameBytes_);
        std::memcpy(destination + first * frameBytes_, storage_.data(), (count - first) * frameBytes_);
    }

    std::size_t frameBytes_;
    std::size_t capacity_;
    std::vector<std::uint8_t> storage_;
    std::atomic<std::size_t> written_{0};
    std::atomic<std::size_t> read_{0};
};

}  // namespace aw::platform
