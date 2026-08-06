#include "aw/events/EventBus.hpp"
#include "aw/events/EventQueue.hpp"
#include "aw/events/InputEventSource.hpp"
#include "framework/Test.hpp"

using namespace aw;

AW_TEST(EventQueue, DeliversInOrder) {
    events::EventQueue queue;
    AW_CHECK(queue.post(events::ButtonEvent{core::ButtonId::Play,
                                            core::ButtonAction::Pressed, 1}));
    AW_CHECK(queue.post(events::EncoderEvent{3, 2}));

    const auto first = queue.poll();
    AW_REQUIRE(first.has_value());
    const auto* button = std::get_if<events::ButtonEvent>(&*first);
    AW_REQUIRE(button != nullptr);
    AW_CHECK_EQ(button->id, core::ButtonId::Play);

    const auto second = queue.poll();
    AW_REQUIRE(second.has_value());
    const auto* encoder = std::get_if<events::EncoderEvent>(&*second);
    AW_REQUIRE(encoder != nullptr);
    AW_CHECK_EQ(encoder->delta, 3);

    AW_CHECK(!queue.poll().has_value());
}

AW_TEST(EventQueue, CountsDropsInsteadOfBlocking) {
    events::EventQueue queue;
    std::size_t posted = 0;
    while (queue.post(events::EncoderEvent{1, 0})) {
        ++posted;
        if (posted > events::kEventQueueCapacity + 10) {
            break;  // Guard against a queue that never fills.
        }
    }
    AW_CHECK(queue.droppedCount() >= 1);
}

AW_TEST(EventBus, FansOutToEverySubscriber) {
    events::EventQueue queue;
    events::EventBus bus;
    bus.addSource(queue);

    int firstCount = 0;
    int secondCount = 0;
    bus.subscribe([&firstCount](const events::Event&) { ++firstCount; });
    bus.subscribe([&secondCount](const events::Event&) { ++secondCount; });

    queue.post(events::EncoderEvent{1, 0});
    queue.post(events::EncoderEvent{-1, 0});

    AW_CHECK_EQ(bus.dispatchPending(), std::size_t{2});
    AW_CHECK_EQ(firstCount, 2);
    AW_CHECK_EQ(secondCount, 2);
}

AW_TEST(EventBus, RespectsTheDispatchBudget) {
    events::EventQueue queue;
    events::EventBus bus;
    bus.addSource(queue);

    int delivered = 0;
    bus.subscribe([&delivered](const events::Event&) { ++delivered; });

    for (int i = 0; i < 10; ++i) {
        queue.post(events::EncoderEvent{1, 0});
    }

    AW_CHECK_EQ(bus.dispatchPending(4), std::size_t{4});
    AW_CHECK_EQ(delivered, 4);
    AW_CHECK_EQ(bus.dispatchPending(), std::size_t{6});
    AW_CHECK_EQ(delivered, 10);
}

AW_TEST(EventBus, UnsubscribeStopsDelivery) {
    events::EventQueue queue;
    events::EventBus bus;
    bus.addSource(queue);

    int count = 0;
    const auto id = bus.subscribe([&count](const events::Event&) { ++count; });

    queue.post(events::EncoderEvent{1, 0});
    bus.dispatchPending();
    AW_CHECK_EQ(count, 1);

    bus.unsubscribe(id);
    queue.post(events::EncoderEvent{1, 0});
    bus.dispatchPending();
    AW_CHECK_EQ(count, 1);
}

AW_TEST(InputEventSource, TranslatesHardwareCallbacksIntoEvents) {
    events::EventQueue queue;
    events::InputEventSource source{queue};

    // Called as the HAL would call it, from an input thread.
    source.onButtonEvent(core::ButtonId::Record, core::ButtonAction::LongPressed, 1234);
    source.onEncoderTurned(-2, 5678);

    const auto first = queue.poll();
    AW_REQUIRE(first.has_value());
    const auto* button = std::get_if<events::ButtonEvent>(&*first);
    AW_REQUIRE(button != nullptr);
    AW_CHECK_EQ(button->id, core::ButtonId::Record);
    AW_CHECK_EQ(button->action, core::ButtonAction::LongPressed);
    AW_CHECK_EQ(button->timestampMicros, std::uint64_t{1234});

    const auto second = queue.poll();
    AW_REQUIRE(second.has_value());
    const auto* encoder = std::get_if<events::EncoderEvent>(&*second);
    AW_REQUIRE(encoder != nullptr);
    AW_CHECK_EQ(encoder->delta, -2);
}
