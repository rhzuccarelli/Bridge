#include "aw/audio/SampleConverter.hpp"

// Phase 2 implements the conversion routines.  The signatures are fixed now
// because WavReader, WavWriter and the ALSA backend are all written against them.

namespace aw::audio::convert {

core::Status toFloat(std::span<const std::byte> /*source*/,
                     const core::AudioFormat& /*format*/,
                     std::span<std::span<float>> /*destination*/,
                     core::FrameCount /*frames*/) {
    return core::notImplemented("convert::toFloat (Phase 2)");
}

core::Status fromFloat(std::span<const std::span<const float>> /*source*/,
                       const core::AudioFormat& /*format*/,
                       std::span<std::byte> /*destination*/,
                       core::FrameCount /*frames*/) {
    return core::notImplemented("convert::fromFloat (Phase 2)");
}

float int24ToFloat(std::byte /*b0*/, std::byte /*b1*/, std::byte /*b2*/) noexcept {
    return 0.0F;  // Phase 2.
}

void floatToInt24(float /*value*/, std::span<std::byte, 3> /*destination*/) noexcept {
    // Phase 2.
}

}  // namespace aw::audio::convert
