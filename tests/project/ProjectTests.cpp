#include "aw/project/Project.hpp"
#include "aw/project/Timeline.hpp"
#include "aw/project/UndoStack.hpp"
#include "framework/Test.hpp"

using namespace aw;

namespace {

/// A command that renames a track.  Small enough to be obviously correct, which
/// is what makes it useful for testing the stack rather than a command.
class RenameTrackCommand final : public project::Command {
public:
    RenameTrackCommand(core::TrackId id, std::string newName)
        : id_{id}, newName_{std::move(newName)} {}

    [[nodiscard]] std::string_view description() const noexcept override { return "Rename Track"; }

    core::Status apply(project::Project& project) override {
        project::Track* track = project.mutableTimeline().findTrack(id_);
        if (track == nullptr) {
            return core::makeError(core::ErrorCode::NotFound, "track");
        }
        previousName_ = track->name;
        track->name = newName_;
        return core::success();
    }

    core::Status revert(project::Project& project) override {
        project::Track* track = project.mutableTimeline().findTrack(id_);
        if (track == nullptr) {
            return core::makeError(core::ErrorCode::NotFound, "track");
        }
        track->name = previousName_;
        return core::success();
    }

    [[nodiscard]] bool requiresSceneRebuild() const noexcept override { return false; }

private:
    core::TrackId id_;
    std::string newName_;
    std::string previousName_;
};

/// A command that always fails on apply, to check the stack stays clean.
class FailingCommand final : public project::Command {
public:
    [[nodiscard]] std::string_view description() const noexcept override { return "Fail"; }
    core::Status apply(project::Project&) override {
        return core::makeError(core::ErrorCode::IoError, "deliberate");
    }
    core::Status revert(project::Project&) override { return core::success(); }
};

project::Project makeProjectWithOneTrack() {
    project::Project project{"Test", "/projects/Test"};
    project::Track track;
    track.id = project.allocateTrackId();
    track.name = "Track01";
    project.mutableTimeline().tracks.push_back(std::move(track));
    return project;
}

}  // namespace

AW_TEST(Timeline, LengthIsTheFurthestClipEnd) {
    project::Timeline timeline;

    project::Track first;
    first.id = 1;
    first.clips.push_back(project::Clip{1, "a.wav", core::FrameRange{0, 1000}, 0, 1.0F, 0, 0});

    project::Track second;
    second.id = 2;
    second.clips.push_back(project::Clip{2, "b.wav", core::FrameRange{0, 500}, 4000, 1.0F, 0, 0});

    timeline.tracks.push_back(std::move(first));
    timeline.tracks.push_back(std::move(second));

    AW_CHECK_EQ(timeline.lengthFrames(), core::FrameCount{4500});
}

AW_TEST(Timeline, FindsTracksAndDetectsSolo) {
    project::Timeline timeline;
    project::Track track;
    track.id = 7;
    track.name = "Vocals";
    timeline.tracks.push_back(std::move(track));

    AW_REQUIRE(timeline.findTrack(7) != nullptr);
    AW_CHECK_EQ(timeline.findTrack(7)->name, std::string{"Vocals"});
    AW_CHECK(timeline.findTrack(99) == nullptr);

    AW_CHECK(!timeline.hasSolo());
    timeline.findTrack(7)->soloed = true;
    AW_CHECK(timeline.hasSolo());
}

AW_TEST(Track, ReportsTheClipUnderAPosition) {
    project::Track track;
    track.clips.push_back(project::Clip{1, "a.wav", core::FrameRange{0, 100}, 50, 1.0F, 0, 0});

    AW_CHECK(track.clipAt(49) == nullptr);
    AW_REQUIRE(track.clipAt(50) != nullptr);
    AW_CHECK_EQ(track.clipAt(50)->id, core::ClipId{1});
    AW_CHECK(track.clipAt(150) == nullptr);  // Half open: end is exclusive.
}

AW_TEST(UndoStack, UndoRestoresThePreviousState) {
    project::Project project = makeProjectWithOneTrack();
    project::UndoStack undo;

    AW_REQUIRE(static_cast<bool>(
        undo.execute(std::make_unique<RenameTrackCommand>(1, "Guitar"), project)));
    AW_CHECK_EQ(project.timeline().findTrack(1)->name, std::string{"Guitar"});
    AW_CHECK(undo.canUndo());
    AW_CHECK_EQ(std::string{undo.undoDescription()}, std::string{"Rename Track"});

    AW_REQUIRE(static_cast<bool>(undo.undo(project)));
    AW_CHECK_EQ(project.timeline().findTrack(1)->name, std::string{"Track01"});
    AW_CHECK(!undo.canUndo());
    AW_CHECK(undo.canRedo());

    AW_REQUIRE(static_cast<bool>(undo.redo(project)));
    AW_CHECK_EQ(project.timeline().findTrack(1)->name, std::string{"Guitar"});
}

AW_TEST(UndoStack, ANewEditClearsTheRedoBranch) {
    project::Project project = makeProjectWithOneTrack();
    project::UndoStack undo;

    AW_REQUIRE(static_cast<bool>(
        undo.execute(std::make_unique<RenameTrackCommand>(1, "A"), project)));
    AW_REQUIRE(static_cast<bool>(undo.undo(project)));
    AW_CHECK(undo.canRedo());

    AW_REQUIRE(static_cast<bool>(
        undo.execute(std::make_unique<RenameTrackCommand>(1, "B"), project)));
    AW_CHECK(!undo.canRedo());
}

AW_TEST(UndoStack, AFailedCommandIsNotPushed) {
    project::Project project = makeProjectWithOneTrack();
    project::UndoStack undo;

    const auto status = undo.execute(std::make_unique<FailingCommand>(), project);
    AW_CHECK(!status.hasValue());
    AW_CHECK(!undo.canUndo());
    AW_CHECK_EQ(undo.depth(), std::size_t{0});
}

AW_TEST(UndoStack, IsBounded) {
    project::Project project = makeProjectWithOneTrack();
    project::UndoStack undo;

    for (std::size_t i = 0; i < project::UndoStack::kMaxDepth + 10; ++i) {
        AW_REQUIRE(static_cast<bool>(
            undo.execute(std::make_unique<RenameTrackCommand>(1, "n" + std::to_string(i)),
                         project)));
    }
    AW_CHECK_EQ(undo.depth(), project::UndoStack::kMaxDepth);
}

AW_TEST(UndoStack, UndoOnAnEmptyStackFails) {
    project::Project project = makeProjectWithOneTrack();
    project::UndoStack undo;

    const auto status = undo.undo(project);
    AW_REQUIRE(!status.hasValue());
    AW_CHECK_EQ(status.error().code(), core::ErrorCode::InvalidState);
}

AW_TEST(Project, TracksModificationAndRevision) {
    project::Project project = makeProjectWithOneTrack();
    AW_CHECK(!project.isModified());
    AW_CHECK_EQ(project.revision(), std::uint64_t{0});

    project.markModified();
    project.bumpRevision();
    AW_CHECK(project.isModified());
    AW_CHECK_EQ(project.revision(), std::uint64_t{1});

    project.markSaved();
    AW_CHECK(!project.isModified());
}

AW_TEST(Project, HandsOutUniqueIdentifiers) {
    project::Project project{"Test", "/projects/Test"};
    AW_CHECK_EQ(project.allocateTrackId(), core::TrackId{1});
    AW_CHECK_EQ(project.allocateTrackId(), core::TrackId{2});
    AW_CHECK_EQ(project.allocateClipId(), core::ClipId{1});
}
