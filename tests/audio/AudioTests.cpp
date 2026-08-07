#include "aw/audio/AudioBuffer.hpp"
#include "aw/audio/AudioEngine.hpp"
#include "aw/audio/Mixer.hpp"
#include "aw/audio/SampleConverter.hpp"
#include "aw/audio/TrackPlayer.hpp"
#include "aw/audio/Transport.hpp"
#include "aw/audio/WavReader.hpp"
#include "aw/audio/WavWriter.hpp"
#include "aw/events/EventQueue.hpp"
#include "aw/hal/AudioOutputInterface.hpp"
#include "framework/Test.hpp"
#include "support/MemoryStorage.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

using namespace aw;

namespace {

template <typename T>
void appendLittle(std::vector<std::byte>& bytes, T value) {
    const auto* raw = reinterpret_cast<const std::byte*>(&value);
    bytes.insert(bytes.end(), raw, raw + sizeof(T));
}

std::vector<std::byte> makeChunk(std::string_view id, std::vector<std::byte> payload) {
    std::vector<std::byte> bytes;
    bytes.insert(bytes.end(), reinterpret_cast<const std::byte*>(id.data()),
                 reinterpret_cast<const std::byte*>(id.data()) + 4);
    appendLittle<std::uint32_t>(bytes, static_cast<std::uint32_t>(payload.size()));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    if ((payload.size() & 1U) != 0U) {
        bytes.push_back(std::byte{0});
    }
    return bytes;
}

std::vector<std::byte> makeFmtChunk(std::uint16_t formatCode,
                                    std::uint16_t channels,
                                    std::uint32_t sampleRate,
                                    std::uint16_t bitsPerSample) {
    std::vector<std::byte> fmt;
    appendLittle<std::uint16_t>(fmt, formatCode);
    appendLittle<std::uint16_t>(fmt, channels);
    appendLittle<std::uint32_t>(fmt, sampleRate);
    const std::uint16_t blockAlign = static_cast<std::uint16_t>(channels * (bitsPerSample / 8U));
    appendLittle<std::uint32_t>(fmt, sampleRate * blockAlign);
    appendLittle<std::uint16_t>(fmt, blockAlign);
    appendLittle<std::uint16_t>(fmt, bitsPerSample);
    return makeChunk("fmt ", std::move(fmt));
}

std::vector<std::byte> makeInfoListChunk(std::string_view title) {
    std::vector<std::byte> info;
    info.insert(info.end(), {std::byte{'I'}, std::byte{'N'}, std::byte{'F'}, std::byte{'O'}});

    std::vector<std::byte> inam(title.size() + 1);
    for (std::size_t i = 0; i < title.size(); ++i) {
        inam[i] = static_cast<std::byte>(title[i]);
    }
    inam.back() = std::byte{0};

    const auto chunk = makeChunk("INAM", std::move(inam));
    info.insert(info.end(), chunk.begin(), chunk.end());
    return makeChunk("LIST", std::move(info));
}

std::array<std::byte, 3> int24Bytes(float value) {
    std::array<std::byte, 3> bytes{};
    audio::convert::floatToInt24(value, std::span<std::byte, 3>{bytes});
    return bytes;
}

std::vector<std::byte> makeStereo24Data(std::initializer_list<std::array<float, 2>> frames) {
    std::vector<std::byte> data;
    data.reserve(frames.size() * 6);
    for (const auto& frame : frames) {
        const auto left = int24Bytes(frame[0]);
        const auto right = int24Bytes(frame[1]);
        data.insert(data.end(), left.begin(), left.end());
        data.insert(data.end(), right.begin(), right.end());
    }
    return data;
}

std::vector<std::byte> makeWaveFile(std::vector<std::vector<std::byte>> chunks) {
    std::vector<std::byte> bytes;
    bytes.insert(bytes.end(),
                 {std::byte{'R'}, std::byte{'I'}, std::byte{'F'}, std::byte{'F'}});
    appendLittle<std::uint32_t>(bytes, 0U);  // Patched below.
    bytes.insert(bytes.end(),
                 {std::byte{'W'}, std::byte{'A'}, std::byte{'V'}, std::byte{'E'}});
    for (const auto& chunk : chunks) {
        bytes.insert(bytes.end(), chunk.begin(), chunk.end());
    }

    const std::uint32_t riffSize = static_cast<std::uint32_t>(bytes.size() - 8);
    const auto* raw = reinterpret_cast<const std::byte*>(&riffSize);
    std::copy(raw, raw + sizeof(riffSize), bytes.begin() + 4);
    return bytes;
}

bool nearlyEqual(float a, float b, float epsilon = 1.0e-4F) {
    return std::fabs(a - b) <= epsilon;
}

class FakeAudioOutput final : public hal::AudioOutputInterface {
public:
    core::Status open(const core::AudioFormat& format,
                      core::FrameCount bufferFrames,
                      hal::AudioRenderCallback& callback) override {
        format_ = format;
        bufferFrames_ = bufferFrames;
        callback_ = &callback;
        return core::success();
    }

    core::Status start() override {
        running_ = true;
        return core::success();
    }

    void stop() noexcept override { running_ = false; }
    void close() noexcept override { running_ = false; callback_ = nullptr; }

    bool isRunning() const noexcept override { return running_; }
    core::AudioFormat format() const noexcept override { return format_; }
    core::FrameCount bufferFrames() const noexcept override { return bufferFrames_; }
    core::FrameCount latencyFrames() const noexcept override { return bufferFrames_ * 2; }

private:
    core::AudioFormat format_{};
    core::FrameCount bufferFrames_{0};
    hal::AudioRenderCallback* callback_{nullptr};
    bool running_{false};
};

}  // namespace

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

AW_TEST(Transport, AdvanceAppliesSeekAndWrapsLoops) {
    audio::Transport transport;
    transport.requestSeek(100);
    AW_CHECK_EQ(transport.advance(64), core::FrameIndex{100});
    AW_CHECK_EQ(transport.position(), core::FrameIndex{100});

    transport.setLoop(core::FrameRange{10, 20}, true);
    transport.requestPlay();
    transport.requestSeek(25);
    AW_CHECK_EQ(transport.advance(10), core::FrameIndex{25});
    AW_CHECK_EQ(transport.position(), core::FrameIndex{15});
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

AW_TEST(TrackPlayer, RendersPrefetchedAudioAtTimelineOffset) {
    awtest::MemoryStorage storage;
    storage.addFile("/projects/Take/audio/Track01.wav",
                    makeWaveFile({makeFmtChunk(1, 2, 48000, 24),
                                  makeChunk("data",
                                            makeStereo24Data({{1.0F, 1.0F},
                                                              {0.5F, 0.5F},
                                                              {0.25F, 0.25F}}))}));

    audio::TrackPlayer player;
    AW_REQUIRE(player.setSource(storage, "/projects/Take/audio/Track01.wav"));
    AW_REQUIRE(player.prefetch(100, core::FrameRange{1, 2}));

    audio::AudioBuffer output{2, 4};
    output.clear();
    player.render(output, 99, 4);

    AW_CHECK(nearlyEqual(output.channel(0)[0], 0.0F));
    AW_CHECK(nearlyEqual(output.channel(0)[1], 0.5F));
    AW_CHECK(nearlyEqual(output.channel(0)[2], 0.25F));
    AW_CHECK(nearlyEqual(output.channel(0)[3], 0.0F));
}

AW_TEST(Mixer, HonorsSoloBeforeSumming) {
    awtest::MemoryStorage storage;
    storage.addFile("/projects/Take/audio/A.wav",
                    makeWaveFile({makeFmtChunk(1, 2, 48000, 24),
                                  makeChunk("data", makeStereo24Data({{1.0F, 0.0F}}))}));
    storage.addFile("/projects/Take/audio/B.wav",
                    makeWaveFile({makeFmtChunk(1, 2, 48000, 24),
                                  makeChunk("data", makeStereo24Data({{0.0F, 1.0F}}))}));

    audio::TrackPlayer a;
    audio::TrackPlayer b;
    AW_REQUIRE(a.setSource(storage, "/projects/Take/audio/A.wav"));
    AW_REQUIRE(b.setSource(storage, "/projects/Take/audio/B.wav"));
    AW_REQUIRE(a.prefetch(0));
    AW_REQUIRE(b.prefetch(0));
    a.mix().soloed.store(true);

    audio::Mixer mixer;
    mixer.prepare(2, 8);
    audio::AudioBuffer output{2, 1};
    std::array<audio::TrackPlayer*, 2> tracks{&a, &b};
    mixer.render(tracks, output, 0, 1);

    AW_CHECK(nearlyEqual(output.channel(0)[0], 1.0F));
    AW_CHECK(nearlyEqual(output.channel(1)[0], 0.0F));
}

AW_TEST(SampleConverter, Int24RoundTripsExpectedValues) {
    const auto zero = int24Bytes(0.0F);
    AW_CHECK(nearlyEqual(audio::convert::int24ToFloat(zero[0], zero[1], zero[2]), 0.0F));

    const auto half = int24Bytes(0.5F);
    AW_CHECK(nearlyEqual(audio::convert::int24ToFloat(half[0], half[1], half[2]), 0.5F));

    const auto minusOne = int24Bytes(-1.0F);
    AW_CHECK(nearlyEqual(audio::convert::int24ToFloat(minusOne[0], minusOne[1], minusOne[2]),
                         -1.0F));
}

AW_TEST(WavReader, ReadsMetadataFromChunkedStereo24Wave) {
    awtest::MemoryStorage storage;
    storage.addFile("/projects/Take/audio/Track01.wav",
                    makeWaveFile({makeFmtChunk(1, 2, 48000, 24),
                                  makeChunk("JUNK", {std::byte{1}, std::byte{2}, std::byte{3}}),
                                  makeInfoListChunk("Take 01"),
                                  makeChunk("data", makeStereo24Data({{0.0F, 0.0F},
                                                                      {0.5F, -0.5F}}))}));

    const auto metadata = audio::WavReader::readMetadata(storage, "/projects/Take/audio/Track01.wav");
    AW_REQUIRE(metadata.hasValue());
    AW_CHECK_EQ(metadata.value().format.sampleRate, core::SampleRate{48000});
    AW_CHECK_EQ(metadata.value().format.channels, core::ChannelCount{2});
    AW_CHECK_EQ(metadata.value().format.sampleFormat, core::SampleFormat::Int24);
    AW_CHECK_EQ(metadata.value().frameCount, core::FrameCount{2});
    AW_CHECK(metadata.value().isNativeFormat);
    AW_CHECK_EQ(metadata.value().title, std::string{"Take 01"});
}

AW_TEST(WavReader, StreamsStereo24SamplesAndSupportsSeek) {
    awtest::MemoryStorage storage;
    storage.addFile("/projects/Take/audio/Track01.wav",
                    makeWaveFile({makeFmtChunk(1, 2, 48000, 24),
                                  makeChunk("data",
                                            makeStereo24Data({{1.0F, -1.0F},
                                                              {0.5F, -0.5F},
                                                              {0.25F, -0.25F}}))}));

    audio::WavReader reader;
    AW_REQUIRE(reader.open(storage, "/projects/Take/audio/Track01.wav"));
    AW_CHECK(reader.isOpen());

    audio::AudioBuffer buffer{2, 2};
    const auto firstRead = reader.read(buffer);
    AW_REQUIRE(firstRead.hasValue());
    AW_CHECK_EQ(firstRead.value(), core::FrameCount{2});
    AW_CHECK_EQ(reader.position(), core::FrameIndex{2});
    AW_CHECK(nearlyEqual(buffer.channel(0)[0], 1.0F));
    AW_CHECK(nearlyEqual(buffer.channel(1)[0], -1.0F));
    AW_CHECK(nearlyEqual(buffer.channel(0)[1], 0.5F));
    AW_CHECK(nearlyEqual(buffer.channel(1)[1], -0.5F));

    AW_REQUIRE(reader.seek(1));
    AW_CHECK_EQ(reader.position(), core::FrameIndex{1});

    audio::AudioBuffer second{2, 2};
    const auto secondRead = reader.read(second);
    AW_REQUIRE(secondRead.hasValue());
    AW_CHECK_EQ(secondRead.value(), core::FrameCount{2});
    AW_CHECK(nearlyEqual(second.channel(0)[0], 0.5F));
    AW_CHECK(nearlyEqual(second.channel(1)[0], -0.5F));
    AW_CHECK(nearlyEqual(second.channel(0)[1], 0.25F));
    AW_CHECK(nearlyEqual(second.channel(1)[1], -0.25F));
}

AW_TEST(WavReader, RejectsUnsupportedChannelCounts) {
    awtest::MemoryStorage storage;
    storage.addFile("/projects/Take/audio/Bad.wav",
                    makeWaveFile({makeFmtChunk(1, 3, 48000, 24),
                                  makeChunk("data", std::vector<std::byte>(18, std::byte{0}))}));

    audio::WavReader reader;
    const auto status = reader.open(storage, "/projects/Take/audio/Bad.wav");
    AW_REQUIRE(!status.hasValue());
    AW_CHECK_EQ(status.error().code(), core::ErrorCode::UnsupportedFormat);
}

AW_TEST(WavReader, RejectsRiffSizeBeyondPhysicalFile) {
    awtest::MemoryStorage storage;
    auto wave = makeWaveFile({makeFmtChunk(1, 1, 48000, 24),
                              makeChunk("data", std::vector<std::byte>(3, std::byte{0}))});
    const std::uint32_t oversized = static_cast<std::uint32_t>(wave.size() + 100U);
    const auto* raw = reinterpret_cast<const std::byte*>(&oversized);
    std::copy(raw, raw + sizeof(oversized), wave.begin() + 4);
    storage.addFile("/projects/Take/audio/Truncated.wav", std::move(wave));

    const auto metadata =
        audio::WavReader::readMetadata(storage, "/projects/Take/audio/Truncated.wav");
    AW_REQUIRE(!metadata.hasValue());
    AW_CHECK_EQ(metadata.error().code(), core::ErrorCode::InvalidFormat);
}

AW_TEST(WavReader, RejectsChunkThatExceedsRiffBounds) {
    awtest::MemoryStorage storage;
    auto wave = makeWaveFile({makeFmtChunk(1, 1, 48000, 24),
                              makeChunk("data", std::vector<std::byte>(3, std::byte{0}))});
    const std::uint32_t oversizedData = 300U;
    const auto* raw = reinterpret_cast<const std::byte*>(&oversizedData);
    std::copy(raw, raw + sizeof(oversizedData), wave.begin() + 40);
    storage.addFile("/projects/Take/audio/BadChunk.wav", std::move(wave));

    const auto metadata =
        audio::WavReader::readMetadata(storage, "/projects/Take/audio/BadChunk.wav");
    AW_REQUIRE(!metadata.hasValue());
    AW_CHECK_EQ(metadata.error().code(), core::ErrorCode::InvalidFormat);
}

AW_TEST(WavWriter, WritesAPlayableWaveThatRoundTripsThroughReader) {
    awtest::MemoryStorage storage;
    audio::WavWriter writer;
    AW_REQUIRE(writer.create(storage, "/projects/Take/audio/Bounce.wav", core::kNativeFormat));

    audio::AudioBuffer first{2, 2};
    first.channel(0)[0] = -1.0F;
    first.channel(1)[0] = 1.0F;
    first.channel(0)[1] = -0.25F;
    first.channel(1)[1] = 0.25F;

    audio::AudioBuffer second{2, 1};
    second.channel(0)[0] = 0.75F;
    second.channel(1)[0] = -0.75F;

    AW_REQUIRE(writer.write(first));
    AW_REQUIRE(writer.write(second));
    AW_CHECK_EQ(writer.framesWritten(), core::FrameCount{3});
    AW_REQUIRE(writer.close());

    const auto* contents = storage.fileContents("/projects/Take/audio/Bounce.wav");
    AW_REQUIRE(contents != nullptr);
    AW_CHECK(contents->size() >= std::size_t{44});

    audio::WavReader reader;
    AW_REQUIRE(reader.open(storage, "/projects/Take/audio/Bounce.wav"));
    AW_CHECK_EQ(reader.metadata().frameCount, core::FrameCount{3});
    AW_CHECK(reader.metadata().isNativeFormat);

    audio::AudioBuffer readBack{2, 3};
    const auto frames = reader.read(readBack);
    AW_REQUIRE(frames.hasValue());
    AW_CHECK_EQ(frames.value(), core::FrameCount{3});
    AW_CHECK(nearlyEqual(readBack.channel(0)[0], -1.0F));
    AW_CHECK(nearlyEqual(readBack.channel(1)[0], 1.0F));
    AW_CHECK(nearlyEqual(readBack.channel(0)[1], -0.25F));
    AW_CHECK(nearlyEqual(readBack.channel(1)[1], 0.25F));
    AW_CHECK(nearlyEqual(readBack.channel(0)[2], 0.75F));
    AW_CHECK(nearlyEqual(readBack.channel(1)[2], -0.75F));
}

AW_TEST(WavWriter, RejectsMoreThanTwoChannels) {
    awtest::MemoryStorage storage;
    audio::WavWriter writer;
    const core::AudioFormat format{48000, 3, core::SampleFormat::Int24};
    const auto status = writer.create(storage, "/projects/Take/audio/Three.wav", format);
    AW_REQUIRE(!status.hasValue());
    AW_CHECK_EQ(status.error().code(), core::ErrorCode::InvalidArgument);
}

AW_TEST(AudioEngine, RendersSceneAudioIntoInterleavedOutput) {
    awtest::MemoryStorage storage;
    storage.addFile("/projects/Take/audio/Track01.wav",
                    makeWaveFile({makeFmtChunk(1, 2, 48000, 24),
                                  makeChunk("data",
                                            makeStereo24Data({{1.0F, -1.0F},
                                                              {0.5F, -0.5F}}))}));

    FakeAudioOutput output;
    events::EventQueue queue;
    audio::AudioEngine engine{output, storage, queue};

    AW_REQUIRE(engine.start(8));
    audio::EngineScene scene;
    scene.tracks.push_back(
        audio::EngineTrack{1, "/projects/Take/audio/Track01.wav", 0, core::FrameRange{0, 2},
                           1.0F, 0.0F, false, false});
    AW_REQUIRE(engine.setScene(scene));
    engine.transport().setLoop(core::FrameRange{0, 2}, true);
    engine.transport().requestPlay();

    std::array<float, 4> interleaved{};
    engine.renderAudio(interleaved, 2, 2);

    AW_CHECK(nearlyEqual(interleaved[0], 1.0F));
    AW_CHECK(nearlyEqual(interleaved[1], -1.0F));
    AW_CHECK(nearlyEqual(interleaved[2], 0.5F));
    AW_CHECK(nearlyEqual(interleaved[3], -0.5F));

    bool sawTransport = false;
    bool sawLevel = false;
    bool sawPosition = false;
    while (const auto event = queue.poll()) {
        sawTransport = sawTransport || (std::get_if<events::TransportEvent>(&*event) != nullptr);
        sawLevel = sawLevel || (std::get_if<events::LevelEvent>(&*event) != nullptr);
        sawPosition = sawPosition || (std::get_if<events::PositionEvent>(&*event) != nullptr);
    }
    AW_CHECK(sawTransport);
    AW_CHECK(sawLevel);
    AW_CHECK(sawPosition);

    engine.stop();
}
