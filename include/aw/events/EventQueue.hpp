#pragma once

#include <cstddef>
#include <optional>

#include "aw/core/RingBuffer.hpp"
#include "aw/events/Event.hpp"

namespace aw::events {

/// Capacity of one producer's queue.
///
/// 256 events is roughly four seconds of frantic encoder spinning.  If a
/// producer ever fills it, the right response is to drop and count, not to
/// grow: a backlog older than a few frames is stale input the user has already
/// moved on from.
inline constexpr std::size_t kEventQueueCapacity = 256;

/// A single-producer inbox for events.
///
/// Each producer (input backend, audio engine, I/O worker) owns one of these
/// and the main loop drains all of them.  N single-producer queues instead of
/// one multi-producer queue is a deliberate trade: it keeps the wait free
/// implementation trivially correct, and the number of producers is small,
/// fixed, and known at startup.
class EventQueue {
public:
    EventQueue() = default;

    EventQueue(const EventQueue&) = delete;
    EventQueue& operator=(const EventQueue&) = delete;

    /// Producer side.  Wait free.  Returns false if the queue is full, in which
    /// case the event is dropped and `droppedCount()` increments.
    bool post(const Event& event) noexcept {
        if (buffer_.push(event)) {
            return true;
        }
        ++dropped_;
        return false;
    }

    /// Consumer side.  Wait free.
    [[nodiscard]] std::optional<Event> poll() noexcept { return buffer_.pop(); }

    [[nodiscard]] bool empty() const noexcept { return buffer_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }

    /// Diagnostics only - surfaced in the settings screen to catch a producer
    /// that is flooding the system.
    [[nodiscard]] std::size_t droppedCount() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }

private:
    core::RingBuffer<Event, kEventQueueCapacity> buffer_;
    std::atomic<std::size_t> dropped_{0};
};

}  // namespace aw::events
