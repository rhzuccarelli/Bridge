#include "aw/audio/AudioBuffer.hpp"
#include "aw/audio/Mixer.hpp"
#include "aw/audio/Transport.hpp"
#include "aw/audio/WavReader.hpp"
#include "framework/Test.hpp"
#include "support/MemoryStorage.hpp"

using namespace aw;

AW_TEST(AudioBuffer, AllocatesOnceAndResizesWithinCapacity) {
    audio::AudioBuffer buffer{2, 512};
    AW_CHECK_EQ(buffer.channelCount(), core::ChannelCount{2});
    AW_CHECK_EQ(buffer.capacityFrames(), core::FrameCount{512});
    AW_CHECK_EQ(buffer.frameCount(), core::FrameCount{512});

    buffer.setFrameCount(128);
    AW_CHECK_EQ(buffer.frameCount(), core::FrameCount{128});
    AW_CHECK_EQ(buffer.channel(0).size(), std::size_t{128});
    AW_CHECK_EQ(buffer.channel(1).size(), std::size_t{128});
}

AW_TEST(AudioBuffer, ChannelsDoNotOverlap) {
    audio::AudioBuffer buffer{2, 4};
    buffer.clear();

    std::span<float> left = buffer.channel(0);
    std::span<float> right = buffer.channel(1);
    left[0] = 1.0F;

    AW_CHECK_EQ(right[0], 0.0F);
    AW_CHECK_EQ(left[0], 1.0F);
}

AW_TEST(AudioBuffer, ClearZeroesEveryChannel) {
    audio::AudioBuffer buffer{2, 8};
    buffer.channel(0)[3] = 0.5F;
    buffer.channel(1)[7] = -0.5F;

    buffer.clear();
    AW_CHECK_EQ(buffer.channel(0)[3], 0.0F);
    AW_CHECK_EQ(buffer.channel(1)[7], 0.0F);
}

AW_TEST(Transport, StartsStoppedAtZero) {
    audio::Transport transport;
    AW_CHECK_EQ(transport.state(), events::TransportState::Stopped);
    AW_CHECK_EQ(transport.position(), core::FrameIndex{0});
    AW_CHECK(!transport.isLoopEnabled());
}

AW_TEST(Transport, RecordsStateRequests) {
    audio::Transport transport;

    transport.requestPlay();
    AW_CHECK_EQ(transport.state(), events::TransportState::Playing);

    transport.requestPause();
    AW_CHECK_EQ(transport.state(), events::TransportState::Paused);

    transport.requestRecord();
    AW_CHECK_EQ(transport.state(), events::TransportState::Recording);

    transport.requestStop();
    AW_CHECK_EQ(transport.state(), events::TransportState::Stopped);
}

AW_TEST(Transport, StoresTheLoopRange) {
    audio::Transport transport;
    transport.setLoop(core::FrameRange{48000, 96000}, true);

    AW_CHECK(transport.isLoopEnabled());
    AW_CHECK_EQ(transport.loopRange().start, core::FrameIndex{48000});
    AW_CHECK_EQ(transport.loopRange().length, core::FrameCount{96000});
}

AW_TEST(Mixer, RendersSilenceWhenThereAreNoTracks) {
    // The property that must hold from Phase 1 onwards: the output is silent,
    // never stale.
    audio::Mixer mixer;
    mixer.prepare(2, 256);

    audio::AudioBuffer output{2, 256};
    output.channel(0)[0] = 0.9F;  // Stale content from a previous block.

    mixer.render({}, output, 0, 256);

    AW_CHECK_EQ(output.frameCount(), core::FrameCount{256});
    AW_CHECK_EQ(output.channel(0)[0], 0.0F);
    AW_CHECK_EQ(output.channel(1)[255], 0.0F);
}

AW_TEST(Mixer, ClampsMasterGain) {
    audio::Mixer mixer;
    mixer.setMasterGain(-1.0F);
    AW_CHECK_EQ(mixer.masterGain(), 0.0F);

    mixer.setMasterGain(100.0F);
    AW_CHECK_EQ(mixer.masterGain(), 4.0F);
}

AW_TEST(WavReader, ReportsNotImplementedUntilPhaseTwo) {
    // A deliberate marker test: it fails the moment Phase 2 lands, which is the
    // reminder to replace it with real parsing tests.
    awtest::MemoryStorage storage;
    audio::WavReader reader;

    const auto status = reader.open(storage, "/projects/Take/audio/Track01.wav");
    AW_REQUIRE(!status.hasValue());
    AW_CHECK_EQ(status.error().code(), core::ErrorCode::NotImplemented);
    AW_CHECK(!reader.isOpen());
}
