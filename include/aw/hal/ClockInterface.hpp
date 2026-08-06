#pragma once

#include <cstdint>

namespace aw::hal {

/// Monotonic time source.
///
/// Exists so that timing-dependent logic (long press detection, transfer
/// timeouts, UI animation) can be unit tested by driving a fake clock instead
/// of sleeping.  A test that calls `std::this_thread::sleep_for` is a test that
/// will eventually be flaky on a loaded build machine.
class ClockInterface {
public:
    virtual ~ClockInterface() = default;

    ClockInterface(const ClockInterface&) = delete;
    ClockInterface& operator=(const ClockInterface&) = delete;

    /// Microseconds since an arbitrary but fixed origin.  Never goes backwards.
    /// Safe to call from the audio thread.
    [[nodiscard]] virtual std::uint64_t nowMicros() const noexcept = 0;

    /// Blocks the calling thread.  Never call from the audio thread.
    virtual void sleepMicros(std::uint64_t micros) = 0;

    [[nodiscard]] std::uint64_t nowMillis() const noexcept { return nowMicros() / 1000; }

protected:
    ClockInterface() = default;
};

}  // namespace aw::hal
