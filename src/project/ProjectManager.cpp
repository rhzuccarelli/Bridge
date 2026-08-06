#include "aw/project/ProjectManager.hpp"

#include "aw/project/ProjectMetadata.hpp"

// Phase 3 implements project lifecycle and scene building.

namespace aw::project {

ProjectManager::ProjectManager(hal::StorageInterface& storage, events::EventQueue& eventQueue)
    : storage_{storage}, eventQueue_{eventQueue} {}

ProjectManager::~ProjectManager() = default;

core::Status ProjectManager::createProject(const std::string& name) {
    return core::notImplemented("ProjectManager::createProject " + name + " (Phase 3)");
}

core::Status ProjectManager::openProject(const core::Path& root) {
    return core::notImplemented("ProjectManager::openProject " + root.string() + " (Phase 3)");
}

core::Status ProjectManager::saveProject() {
    return core::notImplemented("ProjectManager::saveProject (Phase 3)");
}

core::Status ProjectManager::closeProject() {
    if (!project_) {
        return core::success();
    }
    undoStack_.clear();
    project_.reset();
    eventQueue_.post(events::ProjectEvent{events::ProjectChange::Closed});
    return core::success();
}

core::Result<std::vector<ProjectSummary>> ProjectManager::listProjects() const {
    (void)storage_;
    return core::notImplemented("ProjectManager::listProjects (Phase 3)");
}

core::Status ProjectManager::execute(CommandPtr command) {
    if (!project_) {
        return core::makeError(core::ErrorCode::InvalidState, "no project open");
    }

    const bool needsScene = command && command->requiresSceneRebuild();

    if (auto status = undoStack_.execute(std::move(command), *project_); !status) {
        return status;
    }

    project_->markModified();
    project_->bumpRevision();
    eventQueue_.post(events::ProjectEvent{events::ProjectChange::Modified});

    // Phase 4 hooks the engine up here; the flag is computed now so the
    // decision lives with the command, not with a later caller guessing.
    (void)needsScene;
    return core::success();
}

core::Status ProjectManager::undo() {
    if (!project_) {
        return core::makeError(core::ErrorCode::InvalidState, "no project open");
    }
    if (auto status = undoStack_.undo(*project_); !status) {
        return status;
    }
    project_->markModified();
    project_->bumpRevision();
    eventQueue_.post(events::ProjectEvent{events::ProjectChange::UndoStackChanged});
    return core::success();
}

core::Status ProjectManager::redo() {
    if (!project_) {
        return core::makeError(core::ErrorCode::InvalidState, "no project open");
    }
    if (auto status = undoStack_.redo(*project_); !status) {
        return status;
    }
    project_->markModified();
    project_->bumpRevision();
    eventQueue_.post(events::ProjectEvent{events::ProjectChange::UndoStackChanged});
    return core::success();
}

audio::EngineScene ProjectManager::buildScene() const {
    audio::EngineScene scene;
    if (!project_) {
        return scene;
    }

    // Phase 3/4 fills this in from the timeline's clips.  The shape is already
    // fixed: plain values only, absolute local paths only, no pointers back
    // into the project.
    const Timeline& timeline = project_->timeline();
    scene.sampleRate = timeline.sampleRate;
    scene.lengthFrames = timeline.lengthFrames();
    scene.loopRange = timeline.loopRange;
    scene.loopEnabled = timeline.loopEnabled;
    return scene;
}

}  // namespace aw::project
