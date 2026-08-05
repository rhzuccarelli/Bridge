#include "aw/core/AudioFormat.hpp"
#include "aw/core/Result.hpp"
#include "aw/core/RingBuffer.hpp"
#include "aw/core/Types.hpp"
#include "framework/Test.hpp"

#include <thread>

using namespace aw;

AW_TEST(Result, CarriesAValue) {
    core::Result<int> result{42};
    AW_REQUIRE(result.hasValue());
    AW_CHECK_EQ(result.value(), 42);
}

AW_TEST(Result, CarriesAnError) {
    core::Result<int> result{core::makeError(core::ErrorCode::NotFound, "Track01.wav")};
    AW_REQUIRE(!result.hasValue());
    AW_CHECK_EQ(result.error().code(), core::ErrorCode::NotFound);
    AW_CHECK_EQ(result.error().describe(), std::string{"NotFound: Track01.wav"});
}

AW_TEST(Result, VoidSpecialisationDefaultsToSuccess) {
    core::Status status = core::success();
    AW_CHECK(static_cast<bool>(status));

    core::Status failure = core::makeError(core::ErrorCode::IoError);
    AW_CHECK(!failure.hasValue());
    AW_CHECK_EQ(failure.error().code(), core::ErrorCode::IoError);
}

AW_TEST(RingBuffer, PopOnEmptyReturnsNothing) {
    core::RingBuffer<int, 4> buffer;
    AW_CHECK(buffer.empty());
    AW_CHECK(!buffer.pop().has_value());
}

AW_TEST(RingBuffer, PreservesOrder) {
    core::RingBuffer<int, 8> buffer;
    for (int i = 0; i < 5; ++i) {
        AW_CHECK(buffer.push(i));
    }
    for (int i = 0; i < 5; ++i) {
        const auto value = buffer.pop();
        AW_REQUIRE(value.has_value());
        AW_CHECK_EQ(*value, i);
    }
    AW_CHECK(buffer.empty());
}

AW_TEST(RingBuffer, RefusesToOverflow) {
    core::RingBuffer<int, 4> buffer;  // Usable capacity is 3.
    AW_CHECK_EQ(buffer.capacity(), std::size_t{3});
    AW_CHECK(buffer.push(1));
    AW_CHECK(buffer.push(2));
    AW_CHECK(buffer.push(3));
    AW_CHECK(!buffer.push(4));  // Full: drops rather than blocks or grows.
}

AW_TEST(RingBuffer, SurvivesConcurrentProducerAndConsumer) {
    // The property that matters for the audio thread: no lost or duplicated
    // items when one thread pushes while another pops.
    constexpr int kItemCount = 20000;
    core::RingBuffer<int, 64> buffer;

    std::thread producer{[&buffer] {
        for (int i = 0; i < kItemCount; ++i) {
            while (!buffer.push(i)) {
                std::this_thread::yield();
            }
        }
    }};

    int expected = 0;
    while (expected < kItemCount) {
        if (const auto value = buffer.pop()) {
            if (*value != expected) {
                AW_CHECK_EQ(*value, expected);
                break;
            }
            ++expected;
        }
    }
    producer.join();
    AW_CHECK_EQ(expected, kItemCount);
}

AW_TEST(AudioFormat, KnowsItsFrameSize) {
    constexpr core::AudioFormat stereo24{48000, 2, core::SampleFormat::Int24};
    AW_CHECK_EQ(stereo24.bytesPerFrame(), std::size_t{6});

    constexpr core::AudioFormat mono16{48000, 1, core::SampleFormat::Int16};
    AW_CHECK_EQ(mono16.bytesPerFrame(), std::size_t{2});
}

AW_TEST(Types, ConvertsBetweenFramesAndMillis) {
    AW_CHECK_EQ(core::framesToMillis(48000, 48000), std::int64_t{1000});
    AW_CHECK_EQ(core::millisToFrames(1000, 48000), core::FrameCount{48000});
    AW_CHECK_EQ(core::framesToMillis(0, 0), std::int64_t{0});  // No divide by zero.
}

AW_TEST(Types, FrameRangeIsHalfOpen) {
    constexpr core::FrameRange range{100, 50};
    AW_CHECK_EQ(range.end(), core::FrameIndex{150});
    AW_CHECK(range.contains(100));
    AW_CHECK(range.contains(149));
    AW_CHECK(!range.contains(150));
    AW_CHECK(!range.isEmpty());
}
