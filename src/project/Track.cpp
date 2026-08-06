#include "aw/project/Track.hpp"

#include <algorithm>

namespace aw::project {

core::FrameCount Track::lengthFrames() const noexcept {
    core::FrameIndex end = 0;
    for (const Clip& clip : clips) {
        end = std::max(end, clip.timelineEnd());
    }
    return end;
}

const Clip* Track::clipAt(core::FrameIndex position) const noexcept {
    for (const Clip& clip : clips) {
        if (position >= clip.timelinePosition && position < clip.timelineEnd()) {
            return &clip;
        }
    }
    return nullptr;
}

}  // namespace aw::project
