#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "aw/core/Result.hpp"

namespace aw::project {

class Project;

/// One undoable edit.
///
/// The command pattern, chosen over snapshotting the whole project on every
/// edit.  A snapshot approach is simpler but scales with project size, and on a
/// device with an SD card and modest RAM, "undo costs a full copy" is not
/// acceptable.  A command records only its own inverse - moving a clip stores
/// two frame positions, not a timeline.
///
/// Every command must satisfy: `apply()` then `revert()` leaves the project
/// exactly as it was.  That property is what the Phase 3 test suite asserts,
/// command by command.
class Command {
public:
    virtual ~Command() = default;

    Command(const Command&) = delete;
    Command& operator=(const Command&) = delete;

    /// Short, user facing, shown next to "Undo" on the display.
    [[nodiscard]] virtual std::string_view description() const noexcept = 0;

    [[nodiscard]] virtual core::Status apply(Project& project) = 0;
    [[nodiscard]] virtual core::Status revert(Project& project) = 0;

    /// True when this command changed the set of audio files the engine must
    /// have open, i.e. the engine needs a fresh `EngineScene`.  A gain tweak
    /// returns false and is pushed to the engine as a parameter change instead,
    /// which is what keeps turning the encoder from rebuilding the world.
    [[nodiscard]] virtual bool requiresSceneRebuild() const noexcept { return true; }

protected:
    Command() = default;
};

using CommandPtr = std::unique_ptr<Command>;

}  // namespace aw::project
