#pragma once

#include <memory>
#include <vector>

#include "aw/hal/ButtonInterface.hpp"

namespace aw::platform {

/// Merges several button backends into one `ButtonInterface`.
///
/// The prototype needs this immediately: the encoder's push switch is a real
/// GPIO line on the KY-040, while the remaining controls are virtual keys on
/// the touch strip until physical switches are wired.  Both must arrive as one
/// event stream, because the application is entitled to assume there is exactly
/// one set of buttons.
///
/// Composition rather than a "backend that knows about other backends": each
/// source stays a plain `ButtonInterface` that neither knows nor cares that it
/// is being combined, and the combination itself is testable with fakes.
///
/// Sources are started in order and stopped in reverse.  A source that fails to
/// start is skipped with the failure reported, not fatal - a missing
/// touchscreen should not stop the encoder from working.
class CompositeButtons final : public hal::ButtonInterface {
public:
    CompositeButtons() = default;
    ~CompositeButtons() override;

    /// Takes ownership.  Call before `start()`.
    void addSource(std::unique_ptr<hal::ButtonInterface> source);

    [[nodiscard]] std::size_t sourceCount() const noexcept { return sources_.size(); }

    /// Number of sources that started successfully after the last `start()`.
    [[nodiscard]] std::size_t startedCount() const noexcept { return startedCount_; }

    void setListener(hal::ButtonListener* listener) noexcept override;

    /// Succeeds when at least one source started; fails only when none did.
    core::Status start() override;
    void stop() noexcept override;

    /// True when any source reports the button held.
    [[nodiscard]] bool isPressed(core::ButtonId id) const noexcept override;

private:
    std::vector<std::unique_ptr<hal::ButtonInterface>> sources_;
    std::size_t startedCount_{0};
};

}  // namespace aw::platform
