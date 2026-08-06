#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

#include "aw/core/Result.hpp"
#include "aw/project/Command.hpp"

namespace aw::project {

class Project;

/// Bounded undo/redo history.
///
/// Bounded on purpose: this is a device, not a workstation with swap.  Beyond
/// `kMaxDepth` the oldest command is dropped.  The user-facing promise is
/// "you can always take back the last several things you did", which is what a
/// hardware recorder needs, rather than an unlimited history nobody scrolls.
///
/// The stack owns the commands and applies them; it never touches the project
/// directly.
class UndoStack {
public:
    static constexpr std::size_t kMaxDepth = 64;

    UndoStack() = default;

    UndoStack(const UndoStack&) = delete;
    UndoStack& operator=(const UndoStack&) = delete;

    /// Applies `command` and, on success, pushes it.  Clears the redo history.
    /// On failure the command is discarded and the project is unchanged.
    [[nodiscard]] core::Status execute(CommandPtr command, Project& project);

    [[nodiscard]] core::Status undo(Project& project);
    [[nodiscard]] core::Status redo(Project& project);

    [[nodiscard]] bool canUndo() const noexcept { return !undoStack_.empty(); }
    [[nodiscard]] bool canRedo() const noexcept { return !redoStack_.empty(); }

    /// For the display: "Undo Trim Clip".  Empty when nothing to undo.
    [[nodiscard]] std::string_view undoDescription() const noexcept;
    [[nodiscard]] std::string_view redoDescription() const noexcept;

    void clear() noexcept;

    [[nodiscard]] std::size_t depth() const noexcept { return undoStack_.size(); }

private:
    std::vector<CommandPtr> undoStack_;
    std::vector<CommandPtr> redoStack_;
};

}  // namespace aw::project
