#include "aw/display/DisplayFlusher.hpp"

namespace aw::display {

DisplayFlusher::DisplayFlusher(hal::DisplayInterface& display, Framebuffer& framebuffer)
    : display_{display}, framebuffer_{framebuffer} {
    // Worst case is a full screen, allocated once so a flush never allocates.
    scratch_.resize(static_cast<std::size_t>(framebuffer.width()) * framebuffer.height());
}

core::Status DisplayFlusher::flush() {
    if (!framebuffer_.hasDamage()) {
        return core::success();
    }

    const hal::Rect area = framebuffer_.extractDamage(scratch_);
    if (area.isEmpty()) {
        framebuffer_.clearDamage();
        return core::success();
    }

    const auto count = static_cast<std::size_t>(area.pixelCount());
    if (auto status = display_.blit(area, std::span<const Color>{scratch_.data(), count});
        !status) {
        return status;
    }

    framebuffer_.clearDamage();
    return display_.present();
}

core::Status DisplayFlusher::flushAll() {
    framebuffer_.markDamaged(hal::Rect{0, 0, static_cast<std::int16_t>(framebuffer_.width()),
                                       static_cast<std::int16_t>(framebuffer_.height())});
    return flush();
}

}  // namespace aw::display
