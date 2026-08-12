///
/// Tests for the development platform backends.
///
/// A **separate executable** from `aw_tests`, deliberately.  The portable suite
/// links no platform code at all, and that property is the architectural claim
/// this project makes; folding these tests into it would quietly destroy the
/// only mechanical check of it.  So hardware-adjacent logic gets its own target.
///
/// Everything tested here is the pure-logic half of a backend - key mapping,
/// panel geometry, source merging - chosen so it runs with no framebuffer, no
/// touchscreen and no terminal.
///
#include "aw/platform/CompositeButtons.hpp"
#include "aw/platform/CompositeEncoder.hpp"
#include "aw/platform/FallbackAudioOutput.hpp"
#include "aw/platform/rpi/AlsaPcm.hpp"
#include "aw/platform/rpi/GpioInput.hpp"
#include "aw/platform/desktop/KeyboardInput.hpp"
#include "aw/platform/desktop/VirtualPanel.hpp"
#include "framework/Test.hpp"

#include <array>
#include <vector>

using namespace aw;
using namespace aw::platform;

AW_TEST(AlsaPcm, ConvertsStereo48ToTp7SixChannel96) {
    const std::array<float, 4> stereo{0.25F, -0.25F, 0.75F, -0.75F};
    std::array<float, 24> tp7{};
    rpi::Tp7MonitorConverterState state;
    rpi::convertStereo48ToTp7Monitor96(stereo, tp7, state);

    AW_CHECK_EQ(tp7[0], 0.0F);
    AW_CHECK_EQ(tp7[1], 0.0F);
    AW_CHECK_EQ(tp7[6], 0.125F);
    AW_CHECK_EQ(tp7[7], -0.125F);
    AW_CHECK_EQ(tp7[12], 0.25F);
    AW_CHECK_EQ(tp7[13], -0.25F);
    AW_CHECK_EQ(tp7[18], 0.5F);
    AW_CHECK_EQ(tp7[19], -0.5F);
    AW_CHECK_EQ(state.previousLeft, 0.75F);
    AW_CHECK_EQ(state.previousRight, -0.75F);
    for (std::size_t frame = 0; frame < 4; ++frame) {
        for (std::size_t channel = 2; channel < 6; ++channel) {
            AW_CHECK_EQ(tp7[frame * 6U + channel], 0.0F);
        }
    }
}

AW_TEST(GpioButton, DebouncesAndGeneratesHoldActions) {
    rpi::ButtonStateMachine button;
    AW_CHECK(!button.update(true, 1'000).has_value());
    AW_CHECK(!button.update(false, 5'000).has_value());
    AW_CHECK(!button.update(true, 10'000).has_value());
    const auto pressed = button.update(true, 30'000);
    AW_REQUIRE(pressed.has_value());
    AW_CHECK_EQ(*pressed, core::ButtonAction::Pressed);
    AW_CHECK(button.isPressed());
    const auto held = button.update(true, 530'000);
    AW_REQUIRE(held.has_value());
    AW_CHECK_EQ(*held, core::ButtonAction::LongPressed);
    const auto repeat = button.update(true, 630'000);
    AW_REQUIRE(repeat.has_value());
    AW_CHECK_EQ(*repeat, core::ButtonAction::Repeat);
    AW_CHECK(!button.update(false, 640'000).has_value());
    const auto released = button.update(false, 660'000);
    AW_REQUIRE(released.has_value());
    AW_CHECK_EQ(*released, core::ButtonAction::Released);
}

AW_TEST(GpioEncoder, CoalescesQuadratureTransitionsIntoDetents) {
    rpi::QuadratureDecoder decoder;
    decoder.reset(false, false);
    AW_CHECK_EQ(decoder.update(false, true), 0);
    AW_CHECK_EQ(decoder.update(true, true), 0);
    AW_CHECK_EQ(decoder.update(true, false), 0);
    AW_CHECK_EQ(decoder.update(false, false), -1);
    AW_CHECK_EQ(decoder.update(true, false), 0);
    AW_CHECK_EQ(decoder.update(true, true), 0);
    AW_CHECK_EQ(decoder.update(false, true), 0);
    AW_CHECK_EQ(decoder.update(false, false), 1);
}

// --- Keyboard mapping -------------------------------------------------------

AW_TEST(KeyMap, MapsTransportKeys) {
    AW_CHECK_EQ(desktop::mapKey(' ').action, desktop::KeyAction::Button);
    AW_CHECK_EQ(desktop::mapKey(' ').button, core::ButtonId::Play);
    AW_CHECK_EQ(desktop::mapKey('s').button, core::ButtonId::Stop);
    AW_CHECK_EQ(desktop::mapKey('r').button, core::ButtonId::Record);
}

AW_TEST(KeyMap, MapsTrackKeysInOrder) {
    AW_CHECK_EQ(desktop::mapKey('1').button, core::ButtonId::Track1);
    AW_CHECK_EQ(desktop::mapKey('2').button, core::ButtonId::Track2);
    AW_CHECK_EQ(desktop::mapKey('3').button, core::ButtonId::Track3);
    AW_CHECK_EQ(desktop::mapKey('4').button, core::ButtonId::Track4);
    AW_CHECK_EQ(desktop::mapKey('5').button, core::ButtonId::Track5);
    AW_CHECK_EQ(desktop::mapKey('6').button, core::ButtonId::Track6);
}

AW_TEST(KeyMap, MapsEncoderToArrowKeys) {
    AW_CHECK_EQ(desktop::mapKey(desktop::kKeyRight).action, desktop::KeyAction::EncoderCw);
    AW_CHECK_EQ(desktop::mapKey(desktop::kKeyDown).action, desktop::KeyAction::EncoderCw);
    AW_CHECK_EQ(desktop::mapKey(desktop::kKeyLeft).action, desktop::KeyAction::EncoderCcw);
    AW_CHECK_EQ(desktop::mapKey(desktop::kKeyUp).action, desktop::KeyAction::EncoderCcw);
    AW_CHECK_EQ(desktop::mapKey(desktop::kKeyEnter).button, core::ButtonId::EncoderPush);
}

AW_TEST(KeyMap, EscapeAndBackspaceBothMeanBack) {
    AW_CHECK_EQ(desktop::mapKey('b').button, core::ButtonId::Back);
    AW_CHECK_EQ(desktop::mapKey(desktop::kKeyEscape).button, core::ButtonId::Back);
    AW_CHECK_EQ(desktop::mapKey(desktop::kKeyBackspace).button, core::ButtonId::Back);
}

AW_TEST(KeyMap, UnmappedKeysDoNothing) {
    AW_CHECK_EQ(desktop::mapKey('z').action, desktop::KeyAction::None);
    AW_CHECK_EQ(desktop::mapKey('9').action, desktop::KeyAction::None);
    AW_CHECK_EQ(desktop::mapKey(0).action, desktop::KeyAction::None);
}

AW_TEST(KeyMap, QuitIsDistinctFromAnyButton) {
    AW_CHECK_EQ(desktop::mapKey('q').action, desktop::KeyAction::Quit);
}

// --- Virtual panel geometry -------------------------------------------------

AW_TEST(VirtualPanel, CentresTheCanvasAndLeavesAStrip) {
    // The prototype's 3.5" HDMI LCD.
    const desktop::VirtualPanelLayout layout{480, 320, 320, 240};

    const hal::Rect canvas = layout.canvasArea();
    AW_CHECK_EQ(canvas.x, std::int16_t{80});  // (480 - 320) / 2
    AW_CHECK_EQ(canvas.y, std::int16_t{0});
    AW_CHECK_EQ(canvas.width, std::int16_t{320});
    AW_CHECK_EQ(canvas.height, std::int16_t{240});

    const hal::Rect strip = layout.keyStripArea();
    AW_CHECK_EQ(strip.y, std::int16_t{240});
    AW_CHECK_EQ(strip.width, std::int16_t{480});
    AW_CHECK_EQ(strip.height, std::int16_t{80});
}

AW_TEST(VirtualPanel, ProvidesFourteenKeysCoveringTheStrip) {
    const desktop::VirtualPanelLayout layout{480, 320, 320, 240};
    AW_REQUIRE(layout.keys().size() == 14);

    // Seven columns, two rows of 40.
    AW_CHECK_EQ(layout.keys()[0].area.width, std::int16_t{68});
    AW_CHECK_EQ(layout.keys()[0].area.height, std::int16_t{40});
    AW_CHECK_EQ(layout.keys()[0].id, core::ButtonId::Play);
    AW_CHECK_EQ(layout.keys()[7].id, core::ButtonId::Track1);
    AW_CHECK_EQ(layout.keys()[7].area.y, std::int16_t{280});  // Second row.
}

AW_TEST(VirtualPanel, HitTestsKeysInTheStrip) {
    const desktop::VirtualPanelLayout layout{480, 320, 320, 240};

    // Top-left key, first row.
    const auto play = layout.hitTest(10, 250);
    AW_REQUIRE(play.has_value());
    AW_CHECK_EQ(*play, core::ButtonId::Play);

    // Third column, first row.
    const auto record = layout.hitTest(170, 250);
    AW_REQUIRE(record.has_value());
    AW_CHECK_EQ(*record, core::ButtonId::Record);

    // First column, second row.
    const auto track1 = layout.hitTest(10, 300);
    AW_REQUIRE(track1.has_value());
    AW_CHECK_EQ(*track1, core::ButtonId::Track1);
}

AW_TEST(VirtualPanel, TapsOnTheCanvasAreIgnored) {
    // The shipping device has no touchscreen; taps on the UI itself must do
    // nothing, or touch idioms will creep into the interaction design.
    const desktop::VirtualPanelLayout layout{480, 320, 320, 240};

    AW_CHECK(!layout.hitTest(240, 120).has_value());  // Middle of the canvas.
    AW_CHECK(!layout.hitTest(0, 0).has_value());      // Canvas border area.
    AW_CHECK(!layout.hitTest(479, 239).has_value());
}

AW_TEST(VirtualPanel, OffersNoKeysWhenThereIsNoRoom) {
    // A panel exactly the size of the canvas leaves no strip.
    const desktop::VirtualPanelLayout layout{320, 240, 320, 240};

    AW_CHECK(layout.keyStripArea().isEmpty());
    AW_CHECK(layout.keys().empty());
    AW_CHECK(!layout.hitTest(160, 120).has_value());
}

AW_TEST(VirtualPanel, ClampsACanvasLargerThanThePanel) {
    const desktop::VirtualPanelLayout layout{240, 160, 320, 240};

    AW_CHECK_EQ(layout.canvasArea().width, std::int16_t{240});
    AW_CHECK_EQ(layout.canvasArea().height, std::int16_t{160});
    AW_CHECK(layout.keys().empty());
}

// --- Composite buttons ------------------------------------------------------

namespace {

/// A button source we can drive and make fail on demand.
class FakeButtons final : public hal::ButtonInterface {
public:
    explicit FakeButtons(bool startSucceeds) : startSucceeds_{startSucceeds} {}

    void setListener(hal::ButtonListener* listener) noexcept override { listener_ = listener; }

    core::Status start() override {
        if (!startSucceeds_) {
            return core::makeError(core::ErrorCode::DeviceNotConnected, "fake");
        }
        started = true;
        return core::success();
    }

    void stop() noexcept override { started = false; }

    [[nodiscard]] bool isPressed(core::ButtonId id) const noexcept override {
        return held_.has_value() && *held_ == id;
    }

    void press(core::ButtonId id) {
        held_ = id;
        if (listener_ != nullptr) {
            listener_->onButtonEvent(id, core::ButtonAction::Pressed, 0);
        }
    }

    bool started{false};

private:
    bool startSucceeds_;
    hal::ButtonListener* listener_{nullptr};
    std::optional<core::ButtonId> held_;
};

/// Records what reached the application.
class RecordingListener final : public hal::ButtonListener {
public:
    void onButtonEvent(core::ButtonId id, core::ButtonAction, std::uint64_t) noexcept override {
        received.push_back(id);
    }
    std::vector<core::ButtonId> received;
};

}  // namespace

AW_TEST(CompositeButtons, MergesEveryeSourceIntoOneStream) {
    auto encoderPush = std::make_unique<FakeButtons>(true);
    auto touchKeys = std::make_unique<FakeButtons>(true);
    FakeButtons* encoderPtr = encoderPush.get();
    FakeButtons* touchPtr = touchKeys.get();

    CompositeButtons composite;
    composite.addSource(std::move(encoderPush));
    composite.addSource(std::move(touchKeys));

    RecordingListener listener;
    composite.setListener(&listener);
    AW_REQUIRE(static_cast<bool>(composite.start()));
    AW_CHECK_EQ(composite.startedCount(), std::size_t{2});

    // The real prototype's split: encoder push on GPIO, everything else on touch.
    encoderPtr->press(core::ButtonId::EncoderPush);
    touchPtr->press(core::ButtonId::Play);

    AW_REQUIRE(listener.received.size() == 2);
    AW_CHECK_EQ(listener.received[0], core::ButtonId::EncoderPush);
    AW_CHECK_EQ(listener.received[1], core::ButtonId::Play);
}

AW_TEST(CompositeButtons, SurvivesASourceThatCannotStart) {
    // A missing touchscreen must not stop the encoder from working.
    auto working = std::make_unique<FakeButtons>(true);
    auto broken = std::make_unique<FakeButtons>(false);
    FakeButtons* workingPtr = working.get();

    CompositeButtons composite;
    composite.addSource(std::move(working));
    composite.addSource(std::move(broken));

    AW_REQUIRE(static_cast<bool>(composite.start()));
    AW_CHECK_EQ(composite.startedCount(), std::size_t{1});
    AW_CHECK(workingPtr->started);
}

AW_TEST(CompositeButtons, FailsOnlyWhenNoSourceStarts) {
    CompositeButtons composite;
    composite.addSource(std::make_unique<FakeButtons>(false));

    const auto status = composite.start();
    AW_REQUIRE(!status.hasValue());
    AW_CHECK_EQ(status.error().code(), core::ErrorCode::DeviceNotConnected);
}

AW_TEST(CompositeButtons, ReportsHeldFromAnySource) {
    auto first = std::make_unique<FakeButtons>(true);
    auto second = std::make_unique<FakeButtons>(true);
    FakeButtons* secondPtr = second.get();

    CompositeButtons composite;
    composite.addSource(std::move(first));
    composite.addSource(std::move(second));
    AW_REQUIRE(static_cast<bool>(composite.start()));

    AW_CHECK(!composite.isPressed(core::ButtonId::Record));
    secondPtr->press(core::ButtonId::Record);
    AW_CHECK(composite.isPressed(core::ButtonId::Record));
}

AW_TEST(CompositeButtons, WithNoSourcesFailsRatherThanPretending) {
    CompositeButtons composite;
    AW_CHECK_EQ(composite.sourceCount(), std::size_t{0});
    AW_CHECK(!composite.start().hasValue());
}

// --- Composite encoder ------------------------------------------------------

namespace {

class FakeEncoder final : public hal::EncoderInterface {
public:
    explicit FakeEncoder(bool startSucceeds) : startSucceeds_{startSucceeds} {}

    void setListener(hal::EncoderListener* listener) noexcept override { listener_ = listener; }

    core::Status start() override {
        if (!startSucceeds_) {
            return core::makeError(core::ErrorCode::NotImplemented, "fake");
        }
        started = true;
        return core::success();
    }
    void stop() noexcept override { started = false; }

    std::int32_t consumeDelta() noexcept override {
        const std::int32_t d = pending_;
        pending_ = 0;
        return d;
    }

    void turn(std::int32_t delta) {
        pending_ += delta;
        if (listener_ != nullptr) {
            listener_->onEncoderTurned(delta, 0);
        }
    }

    bool started{false};

private:
    bool startSucceeds_;
    hal::EncoderListener* listener_{nullptr};
    std::int32_t pending_{0};
};

class RecordingEncoderListener final : public hal::EncoderListener {
public:
    void onEncoderTurned(std::int32_t delta, std::uint64_t) noexcept override { total += delta; }
    std::int32_t total{0};
};

}  // namespace

AW_TEST(CompositeEncoder, SumsMotionFromEverySource) {
    auto gpio = std::make_unique<FakeEncoder>(true);
    auto keys = std::make_unique<FakeEncoder>(true);
    FakeEncoder* gpioPtr = gpio.get();
    FakeEncoder* keysPtr = keys.get();

    CompositeEncoder composite;
    composite.addSource(std::move(gpio));
    composite.addSource(std::move(keys));
    AW_REQUIRE(static_cast<bool>(composite.start()));
    AW_CHECK_EQ(composite.startedCount(), std::size_t{2});

    gpioPtr->turn(3);
    keysPtr->turn(-1);
    AW_CHECK_EQ(composite.consumeDelta(), std::int32_t{2});
    AW_CHECK_EQ(composite.consumeDelta(), std::int32_t{0});  // Drained.
}

AW_TEST(CompositeEncoder, ForwardsTheListenerToEverySource) {
    auto gpio = std::make_unique<FakeEncoder>(true);
    auto keys = std::make_unique<FakeEncoder>(true);
    FakeEncoder* gpioPtr = gpio.get();
    FakeEncoder* keysPtr = keys.get();

    CompositeEncoder composite;
    composite.addSource(std::move(gpio));
    composite.addSource(std::move(keys));

    RecordingEncoderListener listener;
    composite.setListener(&listener);
    AW_REQUIRE(static_cast<bool>(composite.start()));

    gpioPtr->turn(2);
    keysPtr->turn(2);
    AW_CHECK_EQ(listener.total, std::int32_t{4});
}

AW_TEST(CompositeEncoder, SurvivesAStubSourceThatCannotStart) {
    // The real prototype: the GPIO encoder is a Phase 6 stub, the keyboard works.
    auto gpioStub = std::make_unique<FakeEncoder>(false);
    auto keyboard = std::make_unique<FakeEncoder>(true);
    FakeEncoder* keyboardPtr = keyboard.get();

    CompositeEncoder composite;
    composite.addSource(std::move(gpioStub));
    composite.addSource(std::move(keyboard));

    AW_REQUIRE(static_cast<bool>(composite.start()));
    AW_CHECK_EQ(composite.startedCount(), std::size_t{1});
    AW_CHECK(keyboardPtr->started);
}

AW_TEST(CompositeEncoder, FailsOnlyWhenNoSourceStarts) {
    CompositeEncoder composite;
    composite.addSource(std::make_unique<FakeEncoder>(false));
    const auto status = composite.start();
    AW_REQUIRE(!status.hasValue());
    AW_CHECK_EQ(status.error().code(), core::ErrorCode::DeviceNotConnected);
}

namespace {

class TestRenderCallback final : public hal::AudioRenderCallback {
public:
    void renderAudio(std::span<float>, core::FrameCount, core::ChannelCount) noexcept override {}
};

class FakeAudioBackend final : public hal::AudioOutputInterface {
public:
    explicit FakeAudioBackend(bool opens) : opens_{opens} {}

    core::Status open(const core::AudioFormat& format,
                      core::FrameCount frames,
                      hal::AudioRenderCallback&) override {
        ++openCalls;
        format_ = format;
        frames_ = frames;
        return opens_ ? core::success()
                      : core::Status{core::makeError(core::ErrorCode::DeviceNotConnected)};
    }
    core::Status start() override { running_ = true; return core::success(); }
    void stop() noexcept override { running_ = false; }
    void close() noexcept override { running_ = false; }
    bool isRunning() const noexcept override { return running_; }
    core::AudioFormat format() const noexcept override { return format_; }
    core::FrameCount bufferFrames() const noexcept override { return frames_; }
    core::FrameCount latencyFrames() const noexcept override { return frames_ * 2; }

    int openCalls{0};
    void setOpens(bool opens) noexcept { opens_ = opens; }

private:
    bool opens_{false};
    bool running_{false};
    core::AudioFormat format_{};
    core::FrameCount frames_{0};
};

}  // namespace

AW_TEST(FallbackAudioOutput, PrefersThePrimaryBackend) {
    auto primary = std::make_unique<FakeAudioBackend>(true);
    auto fallback = std::make_unique<FakeAudioBackend>(true);
    FakeAudioBackend* primaryPtr = primary.get();
    FakeAudioBackend* fallbackPtr = fallback.get();
    FallbackAudioOutput output{std::move(primary), std::move(fallback)};
    TestRenderCallback callback;

    AW_REQUIRE(output.open(core::kNativeFormat, 256, callback));
    AW_CHECK(!output.isUsingFallback());
    AW_CHECK_EQ(primaryPtr->openCalls, 1);
    AW_CHECK_EQ(fallbackPtr->openCalls, 0);
}

AW_TEST(FallbackAudioOutput, UsesSilentBackendWhenPrimaryCannotOpen) {
    auto primary = std::make_unique<FakeAudioBackend>(false);
    auto fallback = std::make_unique<FakeAudioBackend>(true);
    FakeAudioBackend* fallbackPtr = fallback.get();
    FallbackAudioOutput output{std::move(primary), std::move(fallback)};
    TestRenderCallback callback;

    AW_REQUIRE(output.open(core::kNativeFormat, 256, callback));
    AW_CHECK(output.isUsingFallback());
    AW_CHECK_EQ(fallbackPtr->openCalls, 1);
    AW_REQUIRE(output.start());
    AW_CHECK(output.isRunning());
}

AW_TEST(FallbackAudioOutput, ReconnectsPrimaryAfterStartingSilent) {
    auto primary = std::make_unique<FakeAudioBackend>(false);
    auto fallback = std::make_unique<FakeAudioBackend>(true);
    FakeAudioBackend* primaryPtr = primary.get();
    FallbackAudioOutput output{std::move(primary), std::move(fallback)};
    TestRenderCallback callback;

    AW_REQUIRE(output.open(core::kNativeFormat, 256, callback));
    AW_REQUIRE(output.start());
    AW_CHECK(output.isUsingFallback());

    primaryPtr->setOpens(true);
    AW_REQUIRE(output.refresh());
    AW_CHECK(!output.isUsingFallback());
    AW_CHECK(output.isRunning());
    AW_CHECK_EQ(primaryPtr->openCalls, 2);
}

AW_TEST(AlsaPcm, PacksNormalizedFloatAsSigned24LittleEndian) {
    std::array<std::byte, 3> packed{};
    rpi::packS24_3LE(-1.0F, packed);
    AW_CHECK_EQ(std::to_integer<unsigned int>(packed[0]), 0U);
    AW_CHECK_EQ(std::to_integer<unsigned int>(packed[1]), 0U);
    AW_CHECK_EQ(std::to_integer<unsigned int>(packed[2]), 128U);

    rpi::packS24_3LE(1.0F, packed);
    AW_CHECK_EQ(std::to_integer<unsigned int>(packed[0]), 255U);
    AW_CHECK_EQ(std::to_integer<unsigned int>(packed[1]), 255U);
    AW_CHECK_EQ(std::to_integer<unsigned int>(packed[2]), 127U);
}
