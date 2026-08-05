#include "aw/project/UndoStack.hpp"

#include "aw/project/Project.hpp"

namespace aw::project {

core::Status UndoStack::execute(CommandPtr command, Project& project) {
    if (!command) {
        return core::makeError(core::ErrorCode::InvalidArgument, "null command");
    }

    if (auto status = command->apply(project); !status) {
        return status;  // Command failed: nothing pushed, project untouched.
    }

    redoStack_.clear();  // A new edit invalidates the redo branch.
    undoStack_.push_back(std::move(command));

    if (undoStack_.size() > kMaxDepth) {
        undoStack_.erase(undoStack_.begin());
    }
    return core::success();
}

core::Status UndoStack::undo(Project& project) {
    if (undoStack_.empty()) {
        return core::makeError(core::ErrorCode::InvalidState, "nothing to undo");
    }

    CommandPtr command = std::move(undoStack_.back());
    undoStack_.pop_back();

    if (auto status = command->revert(project); !status) {
        // A revert that fails leaves the project in an unknown state; keeping
        // the command on the stack would invite a second attempt at the same
        // broken transition, so it is dropped and the caller is told.
        return status;
    }

    redoStack_.push_back(std::move(command));
    return core::success();
}

core::Status UndoStack::redo(Project& project) {
    if (redoStack_.empty()) {
        return core::makeError(core::ErrorCode::InvalidState, "nothing to redo");
    }

    CommandPtr command = std::move(redoStack_.back());
    redoStack_.pop_back();

    if (auto status = command->apply(project); !status) {
        return status;
    }

    undoStack_.push_back(std::move(command));
    return core::success();
}

std::string_view UndoStack::undoDescription() const noexcept {
    return undoStack_.empty() ? std::string_view{} : undoStack_.back()->description();
}

std::string_view UndoStack::redoDescription() const noexcept {
    return redoStack_.empty() ? std::string_view{} : redoStack_.back()->description();
}

void UndoStack::clear() noexcept {
    undoStack_.clear();
    redoStack_.clear();
}

}  // namespace aw::project
