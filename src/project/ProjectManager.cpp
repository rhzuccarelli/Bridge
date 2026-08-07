#include "aw/project/ProjectManager.hpp"

#include "aw/project/ProjectMetadata.hpp"
#include "aw/storage/ProjectPaths.hpp"

#include <cstdio>

namespace {

std::string defaultTrackName(std::size_t index) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "Track%02zu", index + 1);
    return std::string{buffer};
}

}  // namespace

namespace aw::project {

ProjectManager::ProjectManager(hal::StorageInterface& storage, events::EventQueue& eventQueue)
    : storage_{storage}, eventQueue_{eventQueue} {}

ProjectManager::~ProjectManager() = default;

core::Status ProjectManager::createProject(const std::string& name) {
    if (name.empty()) {
        return core::makeError(core::ErrorCode::InvalidArgument, "project name is empty");
    }

    const core::Path root = storage_.projectsRoot() / storage::ProjectPaths::sanitiseName(name);
    if (storage_.exists(root)) {
        return core::makeError(core::ErrorCode::AlreadyExists, root.string());
    }

    auto created = std::make_unique<Project>(name, root);
    if (auto status = created->paths().ensureLayout(storage_); !status) {
        return status;
    }

    created->mutableTimeline().tracks.clear();
    for (std::size_t i = 0; i < kTrackCount; ++i) {
        Track track;
        track.id = created->allocateTrackId();
        track.name = defaultTrackName(i);
        created->mutableTimeline().tracks.push_back(std::move(track));
    }

    if (auto status = MetadataSerializer::save(storage_, *created); !status) {
        return status;
    }
    created->markSaved();

    project_ = std::move(created);
    undoStack_.clear();
    eventQueue_.post(events::ProjectEvent{events::ProjectChange::Opened});
    return core::success();
}

core::Status ProjectManager::openProject(const core::Path& root) {
    auto loaded = MetadataSerializer::load(storage_, root);
    if (!loaded) {
        return loaded.error();
    }

    project_ = std::make_unique<Project>(std::move(loaded).value());
    undoStack_.clear();
    eventQueue_.post(events::ProjectEvent{events::ProjectChange::Opened});
    return core::success();
}

core::Status ProjectManager::saveProject() {
    if (!project_) {
        return core::makeError(core::ErrorCode::InvalidState, "no project open");
    }
    if (auto status = MetadataSerializer::save(storage_, *project_); !status) {
        return status;
    }
    project_->markSaved();
    eventQueue_.post(events::ProjectEvent{events::ProjectChange::Saved});
    return core::success();
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
    auto entries = storage_.list(storage_.projectsRoot());
    if (!entries) {
        return entries.error();
    }

    std::vector<ProjectSummary> projects;
    for (const hal::DirectoryEntry& entry : entries.value()) {
        const core::Path root = storage_.projectsRoot() / entry.name;
        if (!entry.isDirectory || !storage::ProjectPaths::isProjectDirectory(storage_, root)) {
            continue;
        }

        auto loaded = MetadataSerializer::load(storage_, root);
        if (!loaded) {
            return loaded.error();
        }

        ProjectSummary summary;
        summary.name = loaded.value().name();
        summary.root = root;
        summary.modifiedUnixSeconds = entry.modifiedUnixSeconds;
        summary.trackCount = static_cast<std::uint32_t>(loaded.value().timeline().tracks.size());
        summary.lengthFrames = loaded.value().timeline().lengthFrames();
        projects.push_back(std::move(summary));
    }
    return projects;
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

    const Timeline& timeline = project_->timeline();
    scene.sampleRate = timeline.sampleRate;
    scene.lengthFrames = timeline.lengthFrames();
    scene.loopRange = timeline.loopRange;
    scene.loopEnabled = timeline.loopEnabled;
    for (const Track& track : timeline.tracks) {
        for (const Clip& clip : track.clips) {
            if (clip.sourceFile.empty() || clip.sourceRange.length == 0) {
                continue;
            }

            audio::EngineTrack engineTrack;
            engineTrack.id = track.id;
            engineTrack.audioFile = project_->paths().audioFile(clip.sourceFile);
            engineTrack.startFrame = clip.timelinePosition;
            engineTrack.sourceRange = clip.sourceRange;
            engineTrack.sourceChannel = clip.sourceChannel;
            engineTrack.sourceChannelCount = clip.sourceChannelCount;
            engineTrack.gain = track.gain * clip.gain;
            engineTrack.pan = track.pan;
            engineTrack.muted = track.muted;
            engineTrack.soloed = track.soloed;
            scene.tracks.push_back(std::move(engineTrack));
        }
    }
    return scene;
}

}  // namespace aw::project
