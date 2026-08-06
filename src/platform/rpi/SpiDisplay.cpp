#include "aw/platform/rpi/SpiDisplay.hpp"

// Phase 7 replaces these stubs with the spidev + panel init sequence.

namespace aw::platform::rpi {

struct SpiDisplay::Impl {
    SpiDisplayConfig config;
    bool open{false};
};

SpiDisplay::SpiDisplay(SpiDisplayConfig config) : impl_{std::make_unique<Impl>()} {
    impl_->config = std::move(config);
}

SpiDisplay::~SpiDisplay() = default;

hal::DisplayInfo SpiDisplay::info() const noexcept {
    return hal::DisplayInfo{impl_->config.width, impl_->config.height, true};
}

core::Status SpiDisplay::open() { return core::notImplemented("SpiDisplay::open (Phase 7)"); }

void SpiDisplay::close() noexcept { impl_->open = false; }

core::Status SpiDisplay::blit(const hal::Rect& /*area*/,
                              std::span<const std::uint16_t> /*pixels*/) {
    return core::notImplemented("SpiDisplay::blit (Phase 7)");
}

core::Status SpiDisplay::present() { return core::success(); }

core::Status SpiDisplay::setBacklight(float /*level*/) {
    return core::notImplemented("SpiDisplay::setBacklight (Phase 7)");
}

}  // namespace aw::platform::rpi
