#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "aw/audio/AudioEngine.hpp"
#include "aw/core/Result.hpp"
#include "aw/core/Types.hpp"
#include "aw/events/EventQueue.hpp"
#include "aw/hal/StorageInterface.hpp"
#include "aw/project/Command.hpp"
#include "aw/project/Project.hpp"
#include "aw/project/UndoStack.hpp"

namespace aw::project {

/// Summary of a project on disk, for the browser screen.
struct ProjectSummary {
    std::string name;
    core::Path root;
    std::int64_t modifiedUnixSeconds{0};
    std::uint32_t trackCount{0};
    core::FrameCount lengthFrames{0};
};

/// Owns the open project and everything that changes it.
///
/// Why this class exists: something has to be the single writer of project
/// state, or the UI, the importer and the engine will each mutate it on their
/// own schedule and the undo history will stop meaning anything.  Every edit in
/// the system goes through `execute()`, which is also the only place that marks
/// the project dirty, bumps its revision and tells the engine to re-scene.
///
/// What it deliberately does *not* do: play audio, draw anything, or talk to
/// USB.  It hands the engine a plain `EngineScene` and posts events; the UI
/// reads the project through a const reference.
class ProjectManager {
public:
    ProjectManager(hal::StorageInterface& storage, events::EventQueue& eventQueue);
    ~ProjectManager();

    ProjectManager(const ProjectManager&) = delete;
    ProjectManager& operator=(const ProjectManager&) = delete;

    // --- Lifecycle -----------------------------------------------------------
    [[nodiscard]] core::Status createProject(const std::string& name);
    [[nodiscard]] core::Status openProject(const core::Path& root);
    [[nodiscard]] core::Status saveProject();
    [[nodiscard]] core::Status closeProject();

    [[nodiscard]] bool hasProject() const noexcept { return project_ != nullptr; }

    /// Precondition: `hasProject()`.  Read-only by design: mutation is via `execute`.
    [[nodiscard]] const Project& project() const noexcept { return *project_; }

    /// Enumerates projects under the storage's projects root.
    [[nodiscard]] core::Result<std::vector<ProjectSummary>> listProjects() const;

    // --- Editing -------------------------------------------------------------
    /// The one entry point for changing a project.
    [[nodiscard]] core::Status execute(CommandPtr command);
    [[nodiscard]] core::Status undo();
    [[nodiscard]] core::Status redo();

    [[nodiscard]] const UndoStack& undoStack() const noexcept { return undoStack_; }

    // --- Engine handoff ------------------------------------------------------
    /// Builds the immutable description the audio engine consumes.  Called after
    /// every scene-affecting edit; cheap enough to rebuild wholesale.
    [[nodiscard]] audio::EngineScene buildScene() const;

private:
    hal::StorageInterface& storage_;
    events::EventQueue& eventQueue_;
    std::unique_ptr<Project> project_;
    UndoStack undoStack_;
};

}  // namespace aw::project
