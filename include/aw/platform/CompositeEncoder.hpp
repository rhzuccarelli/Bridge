#pragma once

#include <memory>
#include <vector>

#include "aw/hal/EncoderInterface.hpp"

namespace aw::platform {

/// Merges several encoder backends into one `EncoderInterface`.
///
/// The counterpart to `CompositeButtons`, and needed for the same reason: on the
/// prototype the real encoder is the GPIO KY-040, but while its Phase 6 driver
/// is still a stub - and whenever the device is driven over SSH - the keyboard
/// arrow keys must stand in. Both feed one encoder so the application never has
/// to care where a detent came from.
///
/// Sources are started in order; one that fails to start is skipped with the
/// failure surfaced, not fatal. Motion is the sum of every source's motion:
/// `consumeDelta` drains them all, and each source's listener is forwarded, so
/// whichever API a consumer uses sees the combined movement.
class CompositeEncoder final : public hal::EncoderInterface {
public:
    CompositeEncoder() = default;
    ~CompositeEncoder() override;

    /// Takes ownership. Call before `start()`.
    void addSource(std::unique_ptr<hal::EncoderInterface> source);

    [[nodiscard]] std::size_t sourceCount() const noexcept { return sources_.size(); }
    [[nodiscard]] std::size_t startedCount() const noexcept { return startedCount_; }

    void setListener(hal::EncoderListener* listener) noexcept override;

    /// Succeeds when at least one source started; fails only when none did.
    core::Status start() override;
    void stop() noexcept override;

    /// Sum of the accumulated motion of every source, draining each.
    [[nodiscard]] std::int32_t consumeDelta() noexcept override;

private:
    std::vector<std::unique_ptr<hal::EncoderInterface>> sources_;
    std::size_t startedCount_{0};
};

}  // namespace aw::platform
