#pragma once

#include <string>

#include "aw/core/Types.hpp"
#include "aw/project/Timeline.hpp"
#include "aw/storage/ProjectPaths.hpp"

namespace aw::project {

/// A project: a name, a place on disk, and a timeline.
///
/// The class is deliberately anaemic.  Loading, saving, undo and import all
/// live in `ProjectManager` and the command layer, because a `Project` that
/// could save itself would need to know about storage, and a `Project` that
/// could edit itself would need to know about undo - and then it would be the
/// god object this architecture is built to avoid.
class Project {
public:
    Project(std::string name, core::Path root);

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    void setName(std::string name) { name_ = std::move(name); }

    [[nodiscard]] const storage::ProjectPaths& paths() const noexcept { return paths_; }

    [[nodiscard]] const Timeline& timeline() const noexcept { return timeline_; }

    /// Mutable access is intentionally narrow: only the command layer and the
    /// loader call this, and both do so while holding responsibility for undo.
    [[nodiscard]] Timeline& mutableTimeline() noexcept { return timeline_; }

    [[nodiscard]] bool isModified() const noexcept { return modified_; }
    void markModified() noexcept { modified_ = true; }
    void markSaved() noexcept { modified_ = false; }

    /// Monotonic counter bumped on every mutation.  The UI uses it to decide
    /// whether a redraw is needed without deep-comparing the timeline.
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    void bumpRevision() noexcept { ++revision_; }

    /// Next free identifiers.  Handed out by the command layer.
    [[nodiscard]] core::TrackId allocateTrackId() noexcept { return ++lastTrackId_; }
    [[nodiscard]] core::ClipId allocateClipId() noexcept { return ++lastClipId_; }

private:
    std::string name_;
    storage::ProjectPaths paths_;
    Timeline timeline_;
    bool modified_{false};
    std::uint64_t revision_{0};
    core::TrackId lastTrackId_{0};
    core::ClipId lastClipId_{0};
};

}  // namespace aw::project
