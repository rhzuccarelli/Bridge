#pragma once

#include <cstddef>
#include <span>

#include "aw/core/AudioFormat.hpp"
#include "aw/core/Result.hpp"
#include "aw/core/Types.hpp"

namespace aw::audio {

/// PCM <-> float conversion.
///
/// Free functions in a namespace rather than a class: there is no state, and a
/// class here would only exist to be a namespace with extra steps.  These are
/// the hottest non-mixing routines in the system (every streamed block passes
/// through), so they are kept trivially inlinable and free of branching on
/// format inside the loop - the format switch happens once, outside.
namespace convert {

/// Interleaved integer PCM -> planar float in [-1, 1].
///
/// `source` must hold `frames * format.channels` samples in `format`.
/// `destination` must have `format.channels` spans of at least `frames`.
/// Returns InvalidArgument on a size mismatch; never allocates.
core::Status toFloat(std::span<const std::byte> source,
                     const core::AudioFormat& format,
                     std::span<std::span<float>> destination,
                     core::FrameCount frames);

/// Planar float -> interleaved integer PCM, for writing files and feeding DACs.
/// Samples outside [-1, 1] are clamped, not wrapped.
core::Status fromFloat(std::span<const std::span<const float>> source,
                       const core::AudioFormat& format,
                       std::span<std::byte> destination,
                       core::FrameCount frames);

/// Single sample helpers, exposed for tests and for the packed 24 bit case that
/// is easy to get wrong.
[[nodiscard]] float int24ToFloat(std::byte b0, std::byte b1, std::byte b2) noexcept;
void floatToInt24(float value, std::span<std::byte, 3> destination) noexcept;

}  // namespace convert
}  // namespace aw::audio
