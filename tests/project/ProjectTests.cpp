#include "aw/events/EventQueue.hpp"
#include "aw/project/Project.hpp"
#include "aw/project/ProjectManager.hpp"
#include "aw/project/ProjectMetadata.hpp"
#include "aw/project/Timeline.hpp"
#include "aw/project/UndoStack.hpp"
#include "aw/project/ImportService.hpp"
#include "aw/project/ExportService.hpp"
#include "aw/project/CopyChannelCommand.hpp"
#include "aw/audio/WavWriter.hpp"
#include "framework/Test.hpp"
#include "support/MemoryStorage.hpp"

#include <array>
#include <algorithm>
#include <bit>

using namespace aw;

namespace {

class TransferMtp final : public hal::MTPInterface {
public:
    core::Status openSession(std::uint32_t) override { open = true; return core::success(); }
    void closeSession() noexcept override { open = false; }
    bool hasSession() const noexcept override { return open; }
    core::Result<hal::MtpDeviceInfo> deviceInfo() const override { return hal::MtpDeviceInfo{}; }
    core::Result<std::vector<hal::MtpStorageInfo>> storages() const override {
        return std::vector<hal::MtpStorageInfo>{{1, "TP-7", 1ULL << 30, 1ULL << 30}};
    }
    core::Result<std::vector<hal::MtpObjectInfo>> listObjects(hal::MtpStorageId,
                                                              hal::MtpObjectHandle) override {
        return objects;
    }
    core::Result<hal::MtpObjectInfo> objectInfo(hal::MtpObjectHandle) override {
        return core::makeError(core::ErrorCode::NotFound, "object");
    }
    core::Status readObject(hal::MtpObjectHandle, const hal::MtpWriteSink& sink,
                            const hal::MtpProgressCallback& progress) override {
        if (!sink(bytes)) return core::makeError(core::ErrorCode::Cancelled, "sink");
        if (!progress(bytes.size(), bytes.size()))
            return core::makeError(core::ErrorCode::Cancelled, "progress");
        return core::success();
    }
    core::Result<hal::MtpObjectHandle> writeObject(hal::MtpStorageId, hal::MtpObjectHandle,
        const std::string& name, std::uint64_t size, const hal::MtpReadSource& source,
        const hal::MtpProgressCallback& progress) override {
        uploadedName = name;
        uploaded.clear();
        std::array<std::byte, 64> buffer{};
        while (uploaded.size() < size) {
            const std::size_t count = source(buffer);
            if (count == 0) break;
            uploaded.insert(uploaded.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(count));
            if (!progress(uploaded.size(), size))
                return core::makeError(core::ErrorCode::Cancelled, "upload");
        }
        return uploaded.size() == size ? core::Result<hal::MtpObjectHandle>{hal::MtpObjectHandle{9}}
                                       : core::Result<hal::MtpObjectHandle>{core::makeError(core::ErrorCode::Corrupt, "size")};
    }
    core::Status deleteObject(hal::MtpObjectHandle handle) override {
        deleted = handle;
        objects.erase(std::remove_if(objects.begin(), objects.end(),
            [handle](const hal::MtpObjectInfo& object) { return object.handle == handle; }), objects.end());
        return core::success();
    }

    bool open{true};
    std::vector<std::byte> bytes;
    std::vector<std::byte> uploaded;
    std::string uploadedName;
    std::vector<hal::MtpObjectInfo> objects;
    hal::MtpObjectHandle deleted{0};
};

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

events::ProjectEvent nextProjectEvent(events::EventQueue& queue) {
    const auto event = queue.poll();
    AW_REQUIRE(event.has_value());
    const auto* projectEvent = std::get_if<events::ProjectEvent>(&*event);
    AW_REQUIRE(projectEvent != nullptr);
    return *projectEvent;
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

AW_TEST(Project, OnlyTheSelectedDeckIsActive) {
    project::Project project{"Set", "/projects/Set"};
    const project::DeckId deckA = project.activeDeckId();
    project::Deck& deckB = project.addDeck("Deck B");
    const project::DeckId deckBId = deckB.id;
    deckB.timeline.loopEnabled = true;

    AW_CHECK_EQ(project.activeDeck().name, std::string{"Deck A"});
    AW_REQUIRE(project.setActiveDeck(deckBId));
    AW_CHECK_EQ(project.activeDeck().name, std::string{"Deck B"});
    AW_CHECK(project.timeline().loopEnabled);
    AW_REQUIRE(project.setActiveDeck(deckA));
    AW_CHECK(!project.timeline().loopEnabled);
}

AW_TEST(MetadataSerializer, SavesAndLoadsProjectState) {
    awtest::MemoryStorage storage;
    project::Project project{"Session A", "/projects/SessionA"};
    AW_REQUIRE(project.paths().ensureLayout(storage));

    project.mutableTimeline().sampleRate = 48000;
    project.mutableTimeline().loopRange = core::FrameRange{128, 256};
    project.mutableTimeline().loopEnabled = true;

    project::Track track;
    track.id = project.allocateTrackId();
    track.name = "Vocals";
    track.gain = 0.75F;
    track.pan = -0.25F;
    track.muted = true;

    project::Clip clip;
    clip.id = project.allocateClipId();
    clip.sourceFile = "take01.wav";
    clip.sourceRange = core::FrameRange{64, 512};
    clip.timelinePosition = 1024;
    clip.gain = 0.5F;
    clip.fadeInFrames = 12;
    clip.fadeOutFrames = 24;
    clip.sourceChannel = 8;
    clip.sourceChannelCount = 2;
    clip.destinationChannel = 1;
    clip.destinationChannelCount = 1;
    track.clips.push_back(std::move(clip));

    project.mutableTimeline().tracks.push_back(std::move(track));

    project::Deck& library = project.addDeck("Deck B");
    library.timeline.sampleRate = 48000;
    project::Track libraryTrack;
    libraryTrack.id = project.allocateTrackId();
    libraryTrack.name = "Library pair 1";
    library.timeline.tracks.push_back(std::move(libraryTrack));
    AW_REQUIRE(project.setActiveDeck(library.id));

    AW_REQUIRE(project::MetadataSerializer::save(storage, project));

    const auto loaded = project::MetadataSerializer::load(storage, "/projects/SessionA");
    AW_REQUIRE(loaded.hasValue());
    AW_CHECK_EQ(loaded.value().name(), std::string{"Session A"});
    AW_CHECK_EQ(loaded.value().timeline().sampleRate, core::SampleRate{48000});
    AW_CHECK(loaded.value().decks()[1].timeline.loopEnabled);
    AW_CHECK_EQ(loaded.value().decks()[1].timeline.loopRange.start, core::FrameIndex{128});
    AW_CHECK_EQ(loaded.value().decks()[1].timeline.loopRange.length, core::FrameCount{256});
    AW_CHECK_EQ(loaded.value().timeline().tracks.size(), std::size_t{1});
    AW_CHECK_EQ(loaded.value().activeDeck().name, std::string{"Deck B"});
    AW_CHECK_EQ(loaded.value().timeline().tracks[0].name, std::string{"Library pair 1"});
    AW_CHECK_EQ(loaded.value().decks()[1].timeline.tracks[0].clips.size(), std::size_t{1});
    AW_CHECK_EQ(loaded.value().decks()[1].timeline.tracks[0].clips[0].sourceFile,
                std::string{"take01.wav"});
    AW_CHECK_EQ(loaded.value().decks()[1].timeline.tracks[0].clips[0].timelinePosition,
                core::FrameIndex{1024});
    AW_CHECK_EQ(loaded.value().decks()[1].timeline.tracks[0].clips[0].sourceChannel,
                core::ChannelCount{8});
    AW_CHECK_EQ(loaded.value().decks()[1].timeline.tracks[0].clips[0].destinationChannel,
                core::ChannelCount{1});
    AW_CHECK_EQ(loaded.value().decks()[1].timeline.tracks[0].clips[0].destinationChannelCount,
                core::ChannelCount{1});
    AW_CHECK_EQ(loaded.value().decks().size(), std::size_t{2});
    AW_CHECK_EQ(loaded.value().decks()[1].name, std::string{"Deck A"});
}

AW_TEST(ProjectManager, CreateSaveOpenAndListProjects) {
    awtest::MemoryStorage storage;
    events::EventQueue queue;
    project::ProjectManager manager{storage, queue};

    AW_REQUIRE(manager.createProject("Session 1"));
    AW_CHECK(manager.hasProject());
    AW_CHECK_EQ(manager.project().name(), std::string{"Session 1"});
    AW_CHECK_EQ(manager.project().timeline().tracks.size(),
                static_cast<std::size_t>(project::kTrackCount));
    AW_CHECK(storage.exists("/projects/Session 1/metadata.json"));
    AW_CHECK_EQ(nextProjectEvent(queue).change, events::ProjectChange::Opened);

    AW_REQUIRE(manager.saveProject());
    AW_CHECK(!manager.project().isModified());
    AW_CHECK_EQ(nextProjectEvent(queue).change, events::ProjectChange::Saved);

    const auto projects = manager.listProjects();
    AW_REQUIRE(projects.hasValue());
    AW_CHECK_EQ(projects.value().size(), std::size_t{1});
    AW_CHECK_EQ(projects.value()[0].name, std::string{"Session 1"});
    AW_CHECK_EQ(projects.value()[0].trackCount, static_cast<std::uint32_t>(project::kTrackCount));

    AW_REQUIRE(manager.closeProject());
    AW_CHECK(!manager.hasProject());
    AW_CHECK_EQ(nextProjectEvent(queue).change, events::ProjectChange::Closed);

    AW_REQUIRE(manager.openProject("/projects/Session 1"));
    AW_CHECK(manager.hasProject());
    AW_CHECK_EQ(manager.project().name(), std::string{"Session 1"});
    AW_CHECK_EQ(nextProjectEvent(queue).change, events::ProjectChange::Opened);
}

AW_TEST(ProjectManager, BuildSceneFlattensTimelineClipsIntoEngineTracks) {
    awtest::MemoryStorage storage;
    events::EventQueue queue;
    project::ProjectManager manager{storage, queue};

    AW_REQUIRE(manager.createProject("Session 2"));
    project::Timeline& timeline =
        const_cast<project::Timeline&>(manager.project().timeline());

    project::Track* track = timeline.findTrack(1);
    AW_REQUIRE(track != nullptr);
    track->gain = 0.5F;
    track->pan = -0.25F;
    track->soloed = true;
    track->clips.push_back(
        project::Clip{1, "take01.wav", core::FrameRange{64, 512}, 1024, 0.75F, 0, 0});

    const audio::EngineScene scene = manager.buildScene();
    AW_CHECK_EQ(scene.sampleRate, core::SampleRate{48000});
    AW_CHECK_EQ(scene.tracks.size(), std::size_t{1});
    AW_CHECK_EQ(scene.tracks[0].id, core::TrackId{1});
    AW_CHECK_EQ(scene.tracks[0].audioFile, core::Path{"/projects/Session 2/audio/take01.wav"});
    AW_CHECK_EQ(scene.tracks[0].startFrame, core::FrameIndex{1024});
    AW_CHECK_EQ(scene.tracks[0].sourceRange.start, core::FrameIndex{64});
    AW_CHECK_EQ(scene.tracks[0].sourceRange.length, core::FrameCount{512});
    AW_CHECK_EQ(scene.tracks[0].gain, 0.375F);
    AW_CHECK_EQ(scene.tracks[0].pan, -0.25F);
    AW_CHECK(scene.tracks[0].soloed);
}

AW_TEST(ProjectManager, BuildSceneIncludesOnlyTheActiveDeck) {
    awtest::MemoryStorage storage;
    events::EventQueue queue;
    project::ProjectManager manager{storage, queue};
    AW_REQUIRE(manager.createProject("Deck selection"));

    project::Project& editable = const_cast<project::Project&>(manager.project());
    editable.mutableTimeline().tracks[0].clips.push_back(
        project::Clip{1, "active.wav", core::FrameRange{0, 10}, 0});
    project::Deck& library = editable.addDeck("Library");
    project::Track sourceTrack;
    sourceTrack.id = editable.allocateTrackId();
    sourceTrack.clips.push_back(project::Clip{2, "silent.wav", core::FrameRange{0, 10}, 0});
    library.timeline.tracks.push_back(std::move(sourceTrack));

    const audio::EngineScene scene = manager.buildScene();
    AW_CHECK_EQ(scene.tracks.size(), std::size_t{1});
    AW_CHECK_EQ(scene.tracks[0].audioFile,
                core::Path{"/projects/Deck selection/audio/active.wav"});
}

AW_TEST(CopyChannelCommand, CopiesDeckATrackFiveLeftIntoDeckCTrackTwoRight) {
    project::Project project{"Routing", "/projects/Routing"};
    auto addSixTracks = [&project](project::Deck& deck) {
        for (std::size_t index = 0; index < project::kTrackCount; ++index) {
            project::Track track;
            track.id = project.allocateTrackId();
            track.name = "Track" + std::to_string(index + 1);
            deck.timeline.tracks.push_back(std::move(track));
        }
    };
    addSixTracks(*project.findDeck(project.activeDeckId()));
    project::Deck& deckC = project.addDeck("Deck C");
    const project::DeckId deckCId = deckC.id;
    addSixTracks(deckC);

    project::Deck* deckA = project.findDeck(project.activeDeckId());
    AW_REQUIRE(deckA != nullptr);
    project::Clip source;
    source.id = project.allocateClipId();
    source.sourceFile = "tp7-a.wav";
    source.sourceRange = {100, 500};
    source.timelinePosition = 200;
    source.sourceChannel = 8;
    source.sourceChannelCount = 2;
    deckA->timeline.tracks[4].clips.push_back(source);

    project::Clip existing;
    existing.id = project.allocateClipId();
    existing.sourceFile = "tp7-c.wav";
    existing.sourceRange = {0, 500};
    existing.sourceChannel = 2;
    existing.sourceChannelCount = 2;
    deckC.timeline.tracks[1].clips.push_back(existing);

    project::CopyChannelCommand command{deckA->id, 4, 0, deckCId, 1, 1};
    AW_REQUIRE(command.apply(project));
    AW_CHECK_EQ(project.activeDeckId(), deckCId);
    const auto& routed = project.activeDeck().timeline.tracks[1].clips;
    AW_REQUIRE(routed.size() == 2);
    AW_CHECK_EQ(routed[0].sourceFile, std::string{"tp7-c.wav"});
    AW_CHECK_EQ(routed[0].sourceChannel, core::ChannelCount{2});
    AW_CHECK_EQ(routed[0].destinationChannel, core::ChannelCount{0});
    AW_CHECK_EQ(routed[0].destinationChannelCount, core::ChannelCount{1});
    AW_CHECK_EQ(routed[1].sourceFile, std::string{"tp7-a.wav"});
    AW_CHECK_EQ(routed[1].sourceChannel, core::ChannelCount{8});
    AW_CHECK_EQ(routed[1].sourceChannelCount, core::ChannelCount{1});
    AW_CHECK_EQ(routed[1].destinationChannel, core::ChannelCount{1});
    AW_CHECK_EQ(routed[1].destinationChannelCount, core::ChannelCount{1});

    AW_REQUIRE(command.revert(project));
    AW_CHECK_EQ(project.activeDeck().name, std::string{"Deck A"});
    const project::Deck* restoredC = project.findDeck(deckCId);
    AW_REQUIRE(restoredC != nullptr);
    AW_REQUIRE(restoredC->timeline.tracks[1].clips.size() == 1);
    AW_CHECK_EQ(restoredC->timeline.tracks[1].clips[0].destinationChannelCount,
                core::ChannelCount{2});
}

AW_TEST(ImportService, ImportsOneTwelveChannelWaveAsSixStereoPairsInOneDeck) {
    awtest::MemoryStorage sourceStorage;
    audio::WavWriter sourceWriter;
    const core::AudioFormat format{48000, 12, core::SampleFormat::Int24};
    AW_REQUIRE(sourceWriter.create(sourceStorage, "/source.wav", format));
    audio::AudioBuffer frame{12, 1};
    for (core::ChannelCount channel = 0; channel < 12; ++channel)
        frame.channel(channel)[0] = static_cast<float>(channel) / 12.0F;
    AW_REQUIRE(sourceWriter.write(frame));
    AW_REQUIRE(sourceWriter.close());

    TransferMtp mtp;
    const auto* source = sourceStorage.fileContents("/source.wav");
    AW_REQUIRE(source != nullptr);
    mtp.bytes = *source;

    awtest::MemoryStorage storage;
    events::EventQueue queue;
    project::ProjectManager manager{storage, queue};
    AW_REQUIRE(manager.createProject("Import test"));
    project::ImportService importer{storage, mtp, manager, queue};
    project::ImportPlan plan;
    plan.items.push_back({76, "CAGE 2M2.wav", mtp.bytes.size(), core::kInvalidTrackId});
    AW_REQUIRE(importer.run(plan, 7));

    AW_CHECK_EQ(manager.project().decks().size(), std::size_t{2});
    const project::Deck& deck = manager.project().activeDeck();
    AW_CHECK_EQ(deck.name, std::string{"CAGE 2M2"});
    AW_REQUIRE(deck.timeline.tracks.size() == project::kTrackCount);
    for (std::size_t index = 0; index < project::kTrackCount; ++index) {
        AW_REQUIRE(deck.timeline.tracks[index].clips.size() == std::size_t{1});
        const project::Clip& clip = deck.timeline.tracks[index].clips[0];
        AW_CHECK_EQ(clip.sourceChannel, static_cast<core::ChannelCount>(index * 2));
        AW_CHECK_EQ(clip.sourceChannelCount, core::ChannelCount{2});
    }
    AW_REQUIRE(manager.undo());
    AW_CHECK_EQ(manager.project().decks().size(), std::size_t{1});
}

AW_TEST(ExportService, ExportsSixStereoTracksAsOneTwelveChannelWave) {
    awtest::MemoryStorage storage;
    events::EventQueue queue;
    project::ProjectManager manager{storage, queue};
    AW_REQUIRE(manager.createProject("Six pairs"));

    audio::WavWriter sourceWriter;
    const core::AudioFormat format{48000, 12, core::SampleFormat::Int24};
    const core::Path sourcePath = manager.project().paths().audioFile("source.wav");
    audio::AcidMetadata sourceAcid;
    const auto tempoBytes = std::bit_cast<std::array<std::byte, 4>>(110.0F);
    std::copy(tempoBytes.begin(), tempoBytes.end(), sourceAcid.bytes.begin() + 20);
    AW_REQUIRE(sourceWriter.create(storage, sourcePath, format, sourceAcid));
    audio::AudioBuffer sourceFrame{12, 1};
    for (core::ChannelCount channel = 0; channel < 12; ++channel)
        sourceFrame.channel(channel)[0] = static_cast<float>(channel + 1) / 12.0F;
    AW_REQUIRE(sourceWriter.write(sourceFrame));
    AW_REQUIRE(sourceWriter.close());

    project::Timeline& timeline = const_cast<project::Timeline&>(manager.project().timeline());
    for (std::size_t index = 0; index < project::kTrackCount; ++index) {
        project::Clip clip;
        clip.id = static_cast<core::ClipId>(index + 1);
        clip.sourceFile = "source.wav";
        clip.sourceRange = {0, 1};
        clip.sourceChannel = static_cast<core::ChannelCount>(index * 2);
        clip.sourceChannelCount = 2;
        timeline.tracks[index].clips.push_back(clip);
    }

    TransferMtp mtp;
    project::ExportService exporter{storage, mtp, manager, queue};
    project::ExportPlan plan;
    plan.targetName = "Six pairs.wav";
    plan.targetStorage = 1;
    AW_REQUIRE(exporter.run(plan, 11));
    AW_CHECK_EQ(mtp.uploadedName, std::string{"Six pairs.wav"});

    awtest::MemoryStorage verification;
    verification.addFile("/export.wav", mtp.uploaded);
    auto metadata = audio::WavReader::readMetadata(verification, "/export.wav");
    AW_REQUIRE(metadata.hasValue());
    AW_CHECK_EQ(metadata.value().format.channels, core::ChannelCount{12});
    AW_CHECK_EQ(metadata.value().format.sampleRate, core::SampleRate{48000});
    AW_CHECK_EQ(metadata.value().format.sampleFormat, core::SampleFormat::Int24);
    AW_CHECK_EQ(metadata.value().frameCount, core::FrameCount{1});
    AW_REQUIRE(metadata.value().acid.has_value());
    AW_CHECK_EQ(metadata.value().acid->tempoBpm(), 110.0F);
    AW_CHECK(metadata.value().acid->bytes == sourceAcid.bytes);

    mtp.objects = {{42, hal::kMtpRootHandle, 1, "Six pairs.wav", mtp.uploaded.size(), false, 0}};
    mtp.uploaded.clear();
    plan.conflictPolicy = project::ExportConflictPolicy::Fail;
    const auto refused = exporter.run(plan, 12);
    AW_REQUIRE(!refused.hasValue());
    AW_CHECK_EQ(refused.error().code(), core::ErrorCode::AlreadyExists);
    AW_CHECK_EQ(mtp.deleted, hal::MtpObjectHandle{0});
    AW_CHECK(mtp.uploaded.empty());

    plan.conflictPolicy = project::ExportConflictPolicy::Overwrite;
    AW_REQUIRE(exporter.run(plan, 13));
    AW_CHECK_EQ(mtp.deleted, hal::MtpObjectHandle{42});
    AW_CHECK(!mtp.uploaded.empty());
}
