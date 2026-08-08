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
#include <bit>
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

std::vector<std::byte> makeRepeatedStereo24Data(std::array<float, 2> frame,
                                                std::size_t frameCount) {
    std::vector<std::byte> data;
    data.reserve(frameCount * 6U);
    const auto left = int24Bytes(frame[0]);
    const auto right = int24Bytes(frame[1]);
    for (std::size_t i = 0; i < frameCount; ++i) {
        data.insert(data.end(), left.begin(), left.end());
        data.insert(data.end(), right.begin(), right.end());
    }
    return data;
}

std::vector<std::byte> makeInterleaved24Data(std::span<const float> samples) {
    std::vector<std::byte> data;
    data.reserve(samples.size() * 3U);
    for (const float sample : samples) {
        const auto bytes = int24Bytes(sample);
        data.insert(data.end(), bytes.begin(), bytes.end());
    }
    return data;
}

std::vector<std::byte> makeRepeatedInterleaved24Data(std::span<const float> frame,
                                                      std::size_t frameCount) {
    std::vector<std::byte> data;
    data.reserve(frame.size() * frameCount * 3U);
    for (std::size_t index = 0; index < frameCount; ++index) {
        const auto encoded = makeInterleaved24Data(frame);
        data.insert(data.end(), encoded.begin(), encoded.end());
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

    transport.requestRecord(3);
    AW_CHECK(transport.isRecordArmed());
    AW_CHECK_EQ(transport.armedTrack(), core::TrackId{3});
    AW_CHECK_EQ(transport.state(), events::TransportState::Paused);

    transport.requestPlay();
    AW_CHECK_EQ(transport.state(), events::TransportState::Recording);

    transport.requestStop();
    AW_CHECK_EQ(transport.state(), events::TransportState::Stopped);
    AW_CHECK(transport.isRecordArmed());

    transport.disarmRecord();
    AW_CHECK(!transport.isRecordArmed());
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

AW_TEST(TrackPlayer, RefillsBeyondItsInitialBoundedWindow) {
    constexpr core::FrameCount kFrames = 10000;
    awtest::MemoryStorage storage;
    storage.addFile("/projects/Take/audio/Long.wav",
                    makeWaveFile({makeFmtChunk(1, 2, 48000, 24),
                                  makeChunk("data", makeRepeatedStereo24Data({0.5F, -0.5F},
                                                                            kFrames))}));
    audio::TrackPlayer player;
    AW_REQUIRE(player.setSource(storage, "/projects/Take/audio/Long.wav"));
    AW_REQUIRE(player.prefetch(0, core::FrameRange{0, kFrames}));

    audio::AudioBuffer output{2, 256};
    for (core::FrameIndex position = 0; position < kFrames; position += 256) {
        const core::FrameCount count = std::min<core::FrameCount>(256, kFrames - position);
        output.setFrameCount(count);
        output.clear();
        player.render(output, position, count);
        AW_CHECK(nearlyEqual(output.channel(0)[0], 0.5F));
        AW_CHECK(nearlyEqual(output.channel(1)[0], -0.5F));
        (void)player.stream();
    }
    AW_CHECK(!player.consumeUnderrunFlag());
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

AW_TEST(WavReader, RejectsMoreThanTwelveChannels) {
    awtest::MemoryStorage storage;
    storage.addFile("/projects/Take/audio/Bad.wav",
                    makeWaveFile({makeFmtChunk(1, 13, 48000, 24),
                                  makeChunk("data", std::vector<std::byte>(39, std::byte{0}))}));

    audio::WavReader reader;
    const auto status = reader.open(storage, "/projects/Take/audio/Bad.wav");
    AW_REQUIRE(!status.hasValue());
    AW_CHECK_EQ(status.error().code(), core::ErrorCode::UnsupportedFormat);
}

AW_TEST(TrackPlayer, SelectsStereoPairFromTwelveChannelTp7Wave) {
    awtest::MemoryStorage storage;
    const std::array<float, 12> frame{0.1F, 0.2F, 0.3F, 0.4F, 0.5F, 0.6F,
                                      0.7F, 0.8F, 0.9F, 1.0F, -0.9F, -0.8F};
    storage.addFile("/projects/Take/audio/TP7.wav",
                    makeWaveFile({makeFmtChunk(1, 12, 48000, 24),
                                  makeChunk("acid", std::vector<std::byte>(24, std::byte{0})),
                                  makeChunk("data", makeInterleaved24Data(frame))}));

    audio::TrackPlayer player;
    AW_REQUIRE(player.setSource(storage, "/projects/Take/audio/TP7.wav", 10, 2));
    AW_REQUIRE(player.prefetch(0));
    audio::AudioBuffer output{2, 1};
    output.clear();
    player.render(output, 0, 1);
    AW_CHECK(nearlyEqual(output.channel(0)[0], -0.9F));
    AW_CHECK(nearlyEqual(output.channel(1)[0], -0.8F));
}

AW_TEST(TrackPlayer, RoutesOneSourceChannelToRightOnly) {
    awtest::MemoryStorage storage;
    const std::array<float, 2> frame{0.25F, -0.75F};
    storage.addFile("/projects/Take/audio/Stereo.wav",
                    makeWaveFile({makeFmtChunk(1, 2, 48000, 24),
                                  makeChunk("data", makeInterleaved24Data(frame))}));
    audio::TrackPlayer player;
    AW_REQUIRE(player.setSource(storage, "/projects/Take/audio/Stereo.wav", 0, 1, 1, 1));
    AW_REQUIRE(player.prefetch(0));
    audio::AudioBuffer output{2, 1};
    output.clear();
    player.render(output, 0, 1);
    AW_CHECK(nearlyEqual(output.channel(0)[0], 0.0F));
    AW_CHECK(nearlyEqual(output.channel(1)[0], 0.25F));
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

AW_TEST(WavWriter, PreservesTp7AcidTempoChunk) {
    awtest::MemoryStorage storage;
    audio::AcidMetadata acid;
    acid.bytes[0] = std::byte{1};
    acid.bytes[4] = std::byte{48};
    acid.bytes[6] = std::byte{0};
    acid.bytes[7] = std::byte{128};
    acid.bytes[16] = std::byte{4};
    acid.bytes[18] = std::byte{4};
    const auto tempo = std::bit_cast<std::array<std::byte, 4>>(110.0F);
    std::copy(tempo.begin(), tempo.end(), acid.bytes.begin() + 20);

    audio::WavWriter writer;
    AW_REQUIRE(writer.create(storage, "/tempo.wav", core::kNativeFormat, acid));
    audio::AudioBuffer frame{2, 1};
    AW_REQUIRE(writer.write(frame));
    AW_REQUIRE(writer.close());

    const auto metadata = audio::WavReader::readMetadata(storage, "/tempo.wav");
    AW_REQUIRE(metadata.hasValue());
    AW_REQUIRE(metadata.value().acid.has_value());
    AW_CHECK_EQ(metadata.value().acid->tempoBpm(), 110.0F);
    AW_CHECK(metadata.value().acid->bytes == acid.bytes);
}

AW_TEST(WavWriter, RejectsMoreThanTwelveChannels) {
    awtest::MemoryStorage storage;
    audio::WavWriter writer;
    const core::AudioFormat format{48000, 13, core::SampleFormat::Int24};
    const auto status = writer.create(storage, "/projects/Take/audio/Thirteen.wav", format);
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

AW_TEST(AudioEngine, StoppedTransportRendersSilence) {
    awtest::MemoryStorage storage;
    storage.addFile("/projects/Take/audio/Track01.wav",
                    makeWaveFile({makeFmtChunk(1, 2, 48000, 24),
                                  makeChunk("data", makeStereo24Data({{1.0F, 1.0F}}))}));
    FakeAudioOutput output;
    events::EventQueue queue;
    audio::AudioEngine engine{output, storage, queue};
    AW_REQUIRE(engine.start(8));
    audio::EngineScene scene;
    scene.tracks.push_back(audio::EngineTrack{
        1, "/projects/Take/audio/Track01.wav", 0, core::FrameRange{0, 1}});
    AW_REQUIRE(engine.setScene(scene));

    std::array<float, 2> interleaved{0.5F, 0.5F};
    engine.renderAudio(interleaved, 1, 2);
    AW_CHECK_EQ(interleaved[0], 0.0F);
    AW_CHECK_EQ(interleaved[1], 0.0F);
    engine.stop();
}

AW_TEST(AudioEngine, WrapsLoopInsideOneOutputBlock) {
    awtest::MemoryStorage storage;
    storage.addFile("/projects/Take/audio/Loop.wav",
                    makeWaveFile({makeFmtChunk(1, 2, 48000, 24),
                                  makeChunk("data", makeStereo24Data({{0.25F, 0.25F},
                                                                      {0.75F, 0.75F}}))}));
    FakeAudioOutput output;
    events::EventQueue queue;
    audio::AudioEngine engine{output, storage, queue};
    AW_REQUIRE(engine.start(8));
    audio::EngineScene scene;
    scene.loopEnabled = true;
    scene.loopRange = core::FrameRange{0, 2};
    scene.tracks.push_back(
        audio::EngineTrack{1, "/projects/Take/audio/Loop.wav", 0, core::FrameRange{0, 2}});
    AW_REQUIRE(engine.setScene(scene));
    engine.transport().requestPlay();

    std::array<float, 6> interleaved{};
    engine.renderAudio(interleaved, 3, 2);
    AW_CHECK(nearlyEqual(interleaved[0], 0.25F));
    AW_CHECK(nearlyEqual(interleaved[2], 0.75F));
    AW_CHECK(nearlyEqual(interleaved[4], 0.25F));
    AW_CHECK_EQ(engine.transport().position(), core::FrameIndex{1});
    engine.stop();
}

AW_TEST(AudioEngine, ActivatesPreparedSceneAtNextBlockBoundary) {
    awtest::MemoryStorage storage;
    storage.addFile("/projects/Take/audio/A.wav",
                    makeWaveFile({makeFmtChunk(1, 2, 48000, 24),
                                  makeChunk("data", makeRepeatedStereo24Data({0.25F, 0.25F}, 2))}));
    storage.addFile("/projects/Take/audio/B.wav",
                    makeWaveFile({makeFmtChunk(1, 2, 48000, 24),
                                  makeChunk("data", makeRepeatedStereo24Data({-0.5F, -0.5F}, 2))}));
    FakeAudioOutput output;
    events::EventQueue queue;
    audio::AudioEngine engine{output, storage, queue};
    AW_REQUIRE(engine.start(8));

    audio::EngineScene first;
    first.tracks.push_back(
        audio::EngineTrack{1, "/projects/Take/audio/A.wav", 0, core::FrameRange{0, 2}});
    AW_REQUIRE(engine.setScene(first));
    engine.transport().requestPlay();
    std::array<float, 2> interleaved{};
    engine.renderAudio(interleaved, 1, 2);
    AW_CHECK(nearlyEqual(interleaved[0], 0.25F));

    audio::EngineScene second;
    second.tracks.push_back(
        audio::EngineTrack{1, "/projects/Take/audio/B.wav", 0, core::FrameRange{0, 2}});
    AW_REQUIRE(engine.setScene(second));
    engine.renderAudio(interleaved, 1, 2);
    AW_CHECK(nearlyEqual(interleaved[0], -0.5F));
    engine.stop();
}

AW_TEST(AudioEngine, MixesSixSynchronizedStereoTracks) {
    awtest::MemoryStorage storage;
    audio::EngineScene scene;
    for (core::TrackId id = 1; id <= 6; ++id) {
        const std::string path = "/projects/Take/audio/Track0" + std::to_string(id) + ".wav";
        storage.addFile(path,
                        makeWaveFile({makeFmtChunk(1, 2, 48000, 24),
                                      makeChunk("data", makeStereo24Data({{0.1F, -0.1F}}))}));
        scene.tracks.push_back(audio::EngineTrack{id, path, 0, core::FrameRange{0, 1}});
    }

    FakeAudioOutput output;
    events::EventQueue queue;
    audio::AudioEngine engine{output, storage, queue};
    AW_REQUIRE(engine.start(8));
    AW_REQUIRE(engine.setScene(scene));
    engine.transport().requestPlay();

    std::array<float, 2> interleaved{};
    engine.renderAudio(interleaved, 1, 2);
    AW_CHECK(nearlyEqual(interleaved[0], 0.6F));
    AW_CHECK(nearlyEqual(interleaved[1], -0.6F));
    engine.stop();
}

AW_TEST(AudioEngine, OpensOneTp7WaveOnceAndDemultiplexesAllSixPairs) {
    constexpr core::FrameCount kFrames = 700;
    awtest::MemoryStorage storage;
    const std::string path = "/projects/Take/audio/TP7-12ch.wav";
    const std::array<float, 12> frame{0.01F, -0.01F, 0.02F, -0.02F, 0.03F, -0.03F,
                                      0.04F, -0.04F, 0.05F, -0.05F, 0.06F, -0.06F};
    storage.addFile(path, makeWaveFile({makeFmtChunk(1, 12, 48000, 24),
                                        makeChunk("data", makeRepeatedInterleaved24Data(
                                                              frame, kFrames))}));

    audio::EngineScene scene;
    for (core::TrackId id = 1; id <= 6; ++id) {
        audio::EngineTrack track;
        track.id = id;
        track.audioFile = path;
        track.sourceRange = core::FrameRange{0, kFrames};
        track.sourceChannel = static_cast<core::ChannelCount>((id - 1) * 2);
        track.sourceChannelCount = 2;
        scene.tracks.push_back(std::move(track));
    }

    FakeAudioOutput output;
    events::EventQueue queue;
    audio::AudioEngine engine{output, storage, queue};
    AW_REQUIRE(engine.start(256));
    AW_REQUIRE(engine.setScene(scene));
    AW_CHECK_EQ(storage.readOpenCount(path), std::size_t{1});
    engine.transport().requestPlay();

    std::array<float, 512> interleaved{};
    for (int block = 0; block < 3; ++block) {
        engine.renderAudio(interleaved, 256, 2);
        AW_CHECK(nearlyEqual(interleaved[0], 0.21F));
        AW_CHECK(nearlyEqual(interleaved[1], -0.21F));
        if (block < 2) {
            AW_CHECK(nearlyEqual(interleaved[510], 0.21F));
            AW_CHECK(nearlyEqual(interleaved[511], -0.21F));
        }
    }
    engine.stop();
}
