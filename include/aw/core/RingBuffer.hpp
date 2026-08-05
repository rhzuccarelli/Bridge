#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <optional>
#include <type_traits>

namespace aw::core {

/// Wait free single-producer / single-consumer queue.
///
/// This is the only sanctioned way to move data across a thread boundary that
/// includes the audio callback.  It never allocates, never locks, and never
/// makes a system call, so the audio thread can touch it without risking a
/// priority inversion or an underrun.
///
/// Constraints, checked at compile time:
///  - `Capacity` is a power of two, so the wrap is a mask rather than a modulo.
///  - `T` is trivially copyable, so a slot can be overwritten without running a
///    destructor the consumer might be reading through.
///
/// One producer thread and one consumer thread.  Two producers is undefined
/// behaviour; if that is ever needed, add a second queue rather than a mutex.
template <typename T, std::size_t Capacity>
class RingBuffer {
    static_assert(Capacity >= 2, "Capacity must be at least 2");
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");

public:
    RingBuffer() = default;

    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    /// Producer side.  Returns false when the queue is full; the caller decides
    /// whether to drop (UI events) or to spin (never, on the audio thread).
    bool push(const T& item) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = (head + 1) & kMask;
        if (next == tail_.load(std::memory_order_acquire)) {
            return false;  // full
        }
        slots_[head] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    /// Consumer side.  Returns nullopt when empty.
    [[nodiscard]] std::optional<T> pop() noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return std::nullopt;  // empty
        }
        T item = slots_[tail];
        tail_.store((tail + 1) & kMask, std::memory_order_release);
        return item;
    }

    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    /// Approximate; exact only when called from the consumer with no concurrent push.
    [[nodiscard]] std::size_t size() const noexcept {
        const std::size_t head = head_.load(std::memory_order_acquire);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        return (head - tail) & kMask;
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity - 1; }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    std::array<T, Capacity> slots_{};
    std::atomic<std::size_t> head_{0};  ///< Written by producer only.
    std::atomic<std::size_t> tail_{0};  ///< Written by consumer only.
};

}  // namespace aw::core
