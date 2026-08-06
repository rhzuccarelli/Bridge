#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "aw/events/Event.hpp"
#include "aw/events/EventQueue.hpp"

namespace aw::events {

/// Handle returned by `subscribe`, used to unsubscribe.
using SubscriptionId = std::uint32_t;
inline constexpr SubscriptionId kInvalidSubscription = 0;

/// Fan-out point between producers and the application.
///
/// The bus owns no threads and does no work of its own: producers push into
/// their own `EventQueue`, and `dispatchPending()` is called from exactly one
/// place - the main loop - which drains every registered queue and calls the
/// subscribers.  That single-threaded delivery guarantee is what lets UI and
/// project code be written without a single mutex.
///
/// Deliberately *not* a publish/subscribe system with per-type topics: with ten
/// event types and a handful of subscribers, a topic registry would be more
/// machinery than the problem deserves.  Subscribers see every event and match
/// with `std::visit` or `std::get_if`.
class EventBus {
public:
    using Handler = std::function<void(const Event&)>;

    EventBus() = default;

    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    /// Registers a producer queue.  The queue must outlive the bus.
    /// Call during startup only; not thread safe against `dispatchPending`.
    void addSource(EventQueue& queue);

    /// Handlers are called in registration order, on the dispatching thread.
    SubscriptionId subscribe(Handler handler);
    void unsubscribe(SubscriptionId id);

    /// Drains every source and delivers to every subscriber.
    /// `budget` caps how many events one pass may deliver, so a flood cannot
    /// starve rendering.  Returns the number delivered.
    std::size_t dispatchPending(std::size_t budget = kEventQueueCapacity);

    /// Delivers immediately, bypassing the queues.  For events synthesised by
    /// the application itself while already on the dispatch thread.
    void publishDirect(const Event& event);

    [[nodiscard]] std::size_t sourceCount() const noexcept { return sources_.size(); }
    [[nodiscard]] std::size_t subscriberCount() const noexcept { return subscribers_.size(); }

private:
    struct Subscriber {
        SubscriptionId id{kInvalidSubscription};
        Handler handler;
    };

    std::vector<EventQueue*> sources_;
    std::vector<Subscriber> subscribers_;
    SubscriptionId nextId_{1};
};

}  // namespace aw::events
