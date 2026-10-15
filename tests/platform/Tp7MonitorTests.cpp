///
/// Tests for the TP-7 USB playback logic shared by the Raspberry Pi and
/// ESP32-P4 backends: feedback decoding, packet sizing and the frame FIFO
/// between the render task and the USB completion callback.
///
#include "aw/platform/Tp7Monitor.hpp"
#include "framework/Test.hpp"

#include <array>
#include <numeric>
#include <thread>
#include <vector>

using namespace aw;
using namespace aw::platform;

AW_TEST(Tp7Monitor, DecodesTheTp7FeedbackFormat) {
    // What a TP-7 on OS 1.1.11 sends: Q10.14 frames per 1 ms frame.
    const auto rate = decodeUac1Feedback(0x180000, kTp7MonitorRate);
    AW_REQUIRE(rate.has_value());
    AW_CHECK_EQ(*rate, 96U << 16);

    const auto slow = decodeUac1Feedback(0x17FF00, kTp7MonitorRate);  // 95.984375
    AW_REQUIRE(slow.has_value());
    AW_CHECK_EQ(*slow, (96U << 16) - 1024U);
}

AW_TEST(Tp7Monitor, DecodesTheOtherFeedbackFormats) {
    const auto perFrame = decodeUac1Feedback(96U << 16, kTp7MonitorRate);
    AW_REQUIRE(perFrame.has_value());
    AW_CHECK_EQ(*perFrame, 96U << 16);

    const auto perMicroframe = decodeUac1Feedback(12U << 16, kTp7MonitorRate);
    AW_REQUIRE(perMicroframe.has_value());
    AW_CHECK_EQ(*perMicroframe, 96U << 16);
}

AW_TEST(Tp7Monitor, RejectsImplausibleFeedback) {
    AW_CHECK(!decodeUac1Feedback(0, kTp7MonitorRate).has_value());
    AW_CHECK(!decodeUac1Feedback(0xFFFFFF, kTp7MonitorRate).has_value());
}

AW_TEST(Tp7Monitor, PacketSizerCarriesTheFraction) {
    IsoPacketSizer sizer{kTp7MonitorRate};
    AW_CHECK_EQ(sizer.next(98), 96U);

    sizer.setRateQ16((96U << 16) + 0x8000);  // 96.5 frames per ms
    std::uint32_t total = 0;
    for (int packet = 0; packet < 1000; ++packet) {
        const std::uint32_t frames = sizer.next(98);
        AW_CHECK(frames == 96U || frames == 97U);
        total += frames;
    }
    AW_CHECK_EQ(total, 96'500U);
}

AW_TEST(Tp7Monitor, PacketSizerClampsToTheEndpoint) {
    IsoPacketSizer sizer{kTp7MonitorRate};
    sizer.setRateQ16(200U << 16);
    AW_CHECK_EQ(sizer.next(98), 98U);
    sizer.setRateQ16(0);
    sizer.reset();
    AW_CHECK_EQ(sizer.next(98), 1U);
}

AW_TEST(Tp7Monitor, FifoMovesWholeFramesAcrossTheWrap) {
    FrameFifo fifo{kTp7FrameBytes, 6};  // Rounded up to 8.
    AW_CHECK_EQ(fifo.capacityFrames(), 8U);

    std::vector<std::uint8_t> in(5 * kTp7FrameBytes);
    std::iota(in.begin(), in.end(), std::uint8_t{0});
    std::vector<std::uint8_t> out(8 * kTp7FrameBytes, 0xEE);

    AW_CHECK_EQ(fifo.write(in), 5U);
    AW_CHECK_EQ(fifo.read(out.data(), 4), 4U);
    AW_CHECK_EQ(fifo.write(in), 5U);  // Frames 5-7, then 0-1 after the wrap.
    AW_CHECK_EQ(fifo.availableFrames(), 6U);
    AW_CHECK_EQ(fifo.read(out.data(), 8), 6U);  // Ran short: underrun.
    AW_CHECK_EQ(out[0], in[4 * kTp7FrameBytes]);
    for (std::size_t byte = 0; byte < in.size(); ++byte) {
        AW_CHECK_EQ(out[kTp7FrameBytes + byte], in[byte]);
    }
    AW_CHECK_EQ(fifo.availableFrames(), 0U);
}

AW_TEST(Tp7Monitor, FifoRefusesWhatDoesNotFit) {
    FrameFifo fifo{2, 4};
    const std::array<std::uint8_t, 12> frames{};
    AW_CHECK_EQ(fifo.write(frames), 4U);  // Six offered, four fit.
    AW_CHECK_EQ(fifo.freeFrames(), 0U);
    AW_CHECK_EQ(fifo.write(frames), 0U);
}

AW_TEST(Tp7Monitor, FifoSurvivesConcurrentProducerAndConsumer) {
    // The render task and the USB completion callback run on different cores.
    constexpr std::uint32_t kFrames = 200'000;
    FrameFifo fifo{4, 256};
    std::thread producer{[&] {
        std::uint32_t next = 0;
        std::array<std::uint32_t, 37> block{};
        while (next < kFrames) {
            const std::uint32_t count = std::min<std::uint32_t>(block.size(), kFrames - next);
            for (std::uint32_t i = 0; i < count; ++i) block[i] = next + i;
            const std::size_t done = fifo.write(
                {reinterpret_cast<const std::uint8_t*>(block.data()), count * 4U});
            next += static_cast<std::uint32_t>(done);
            if (done == 0) std::this_thread::yield();
        }
    }};
    std::uint32_t expected = 0;
    bool ordered = true;
    std::array<std::uint32_t, 98> packet{};
    while (expected < kFrames) {
        const std::size_t got = fifo.read(reinterpret_cast<std::uint8_t*>(packet.data()), packet.size());
        for (std::size_t i = 0; i < got; ++i) ordered = ordered && packet[i] == expected++;
        if (got == 0) std::this_thread::yield();
    }
    producer.join();
    AW_CHECK(ordered);
    AW_CHECK_EQ(fifo.availableFrames(), 0U);
}
