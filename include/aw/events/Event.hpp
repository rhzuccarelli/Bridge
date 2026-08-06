#pragma once

#include <cstdint>
#include <type_traits>
#include <variant>

#include "aw/core/Controls.hpp"
#include "aw/core/Error.hpp"
#include "aw/core/Types.hpp"

namespace aw::events {

/// A physical button changed state.
struct ButtonEvent {
    core::ButtonId id{};
    core::ButtonAction action{};
    std::uint64_t timestampMicros{0};
};

/// The rotary encoder moved.
struct EncoderEvent {
    std::int32_t delta{0};
    std::uint64_t timestampMicros{0};
};

enum class TransportState : std::uint8_t { Stopped, Playing, Recording, Paused };

/// The audio engine's transport changed.  Posted by the engine, consumed by the UI.
struct TransportEvent {
    TransportState state{TransportState::Stopped};
    core::FrameIndex position{0};
};

/// Periodic playhead update, at UI rate rather than audio rate.
struct PositionEvent {
    core::FrameIndex position{0};
};

/// Peak level for one track, for the meters.  Linear, not dB - the UI converts.
struct LevelEvent {
    core::TrackId trackId{core::kInvalidTrackId};
    float peak{0.0F};
};

enum class DeviceKind : std::uint8_t { Mtp, Storage };

/// An external device appeared or disappeared.
struct DeviceEvent {
    DeviceKind kind{DeviceKind::Mtp};
    bool attached{false};
    std::uint32_t deviceKey{0};
};

enum class JobKind : std::uint8_t { Import, Export, Save, Scan };

/// Progress of a long running background job.
struct JobProgressEvent {
    JobKind kind{JobKind::Import};
    std::uint32_t jobId{0};
    std::uint32_t itemsDone{0};
    std::uint32_t itemsTotal{0};
    std::uint8_t percent{0};
};

struct JobFinishedEvent {
    JobKind kind{JobKind::Import};
    std::uint32_t jobId{0};
    bool succeeded{false};
    core::ErrorCode error{core::ErrorCode::Unknown};  ///< Meaningful only on failure.
};

/// The project was modified, saved, opened or closed.
enum class ProjectChange : std::uint8_t { Opened, Closed, Modified, Saved, UndoStackChanged };

struct ProjectEvent {
    ProjectChange change{ProjectChange::Modified};
};

/// Anything the user should be told about that is not a job outcome.
struct NotificationEvent {
    core::ErrorCode code{core::ErrorCode::Unknown};
    bool isError{true};
};

/// The single event type that flows through the system.
///
/// A closed `std::variant` rather than an inheritance hierarchy with
/// `dynamic_cast`, for three reasons: it is trivially copyable (so it fits in a
/// lock free queue with no allocation), the compiler can tell us when a handler
/// forgot a case, and there is no heap involved on a device where the heap is
/// the scarce resource.  The cost is that adding an event type touches this
/// header - acceptable, and arguably a feature, since it forces a review of who
/// should handle it.
using Event = std::variant<ButtonEvent,
                           EncoderEvent,
                           TransportEvent,
                           PositionEvent,
                           LevelEvent,
                           DeviceEvent,
                           JobProgressEvent,
                           JobFinishedEvent,
                           ProjectEvent,
                           NotificationEvent>;

static_assert(std::is_trivially_copyable_v<Event>,
              "Event must stay trivially copyable so it can cross a lock free queue. "
              "Do not add a member that owns memory (std::string, std::vector, ...) - "
              "post an identifier and let the consumer look the payload up instead.");

}  // namespace aw::events
