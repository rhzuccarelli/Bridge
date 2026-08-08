#include "aw/project/ImportService.hpp"

#include "aw/audio/WavReader.hpp"
#include "aw/project/Command.hpp"
#include "aw/storage/ProjectPaths.hpp"

#include <algorithm>
#include <cstdio>
#include <optional>

namespace aw::project {
namespace {

struct ImportedDeck {
    std::string name;
    std::string sourceFile;
    core::ChannelCount channels{0};
    core::FrameCount frames{0};
};

std::string stem(std::string name) {
    const auto dot = name.find_last_of('.');
    if (dot != std::string::npos) name.resize(dot);
    return name.empty() ? std::string{"Imported"} : name;
}

class AddImportedDecksCommand final : public Command {
public:
    explicit AddImportedDecksCommand(std::vector<ImportedDeck> decks) : decks_{std::move(decks)} {}
    std::string_view description() const noexcept override { return "Import TP-7 decks"; }

    core::Status apply(Project& project) override {
        previousActive_ = project.activeDeckId();
        added_.clear();
        for (const ImportedDeck& imported : decks_) {
            Deck& deck = project.addDeck(imported.name);
            added_.push_back(deck.id);
            deck.timeline.tracks.clear();
            for (std::size_t index = 0; index < kTrackCount; ++index) {
                Track track;
                track.id = project.allocateTrackId();
                char label[16];
                std::snprintf(label, sizeof(label), "Track%02zu", index + 1);
                track.name = label;
                const std::size_t firstChannel = index * 2U;
                if (firstChannel < imported.channels) {
                    Clip clip;
                    clip.id = project.allocateClipId();
                    clip.sourceFile = imported.sourceFile;
                    clip.sourceRange = {0, imported.frames};
                    clip.sourceChannel = static_cast<core::ChannelCount>(firstChannel);
                    clip.sourceChannelCount = static_cast<core::ChannelCount>(
                        std::min<std::size_t>(2U, imported.channels - firstChannel));
                    track.clips.push_back(std::move(clip));
                }
                deck.timeline.tracks.push_back(std::move(track));
            }
        }
        return added_.empty() ? core::success() : project.setActiveDeck(added_.front());
    }

    core::Status revert(Project& project) override {
        if (auto status = project.setActiveDeck(previousActive_); !status) return status;
        for (auto it = added_.rbegin(); it != added_.rend(); ++it) {
            if (auto status = project.removeDeck(*it); !status) return status;
        }
        return core::success();
    }

private:
    std::vector<ImportedDeck> decks_;
    std::vector<DeckId> added_;
    DeckId previousActive_{kInvalidDeckId};
};

void postFinished(events::EventQueue& queue, std::uint32_t jobId, const core::Status& status) {
    queue.post(events::JobFinishedEvent{events::JobKind::Import, jobId,
                                        static_cast<bool>(status),
                                        status ? core::ErrorCode::Unknown : status.error().code()});
}

}  // namespace

std::uint64_t ImportPlan::totalBytes() const noexcept {
    std::uint64_t total = 0;
    for (const ImportItem& item : items) total += item.sizeBytes;
    return total;
}

ImportService::ImportService(hal::StorageInterface& storage, hal::MTPInterface& mtp,
                             ProjectManager& projects, events::EventQueue& eventQueue)
    : storage_{storage}, mtp_{mtp}, projects_{projects}, eventQueue_{eventQueue} {}

core::Status ImportService::validate(const ImportPlan& plan) const {
    if (!projects_.hasProject())
        return core::makeError(core::ErrorCode::InvalidState, "open a project before importing");
    if (!mtp_.hasSession())
        return core::makeError(core::ErrorCode::DeviceNotConnected, "TP-7 MTP session is closed");
    if (plan.items.empty())
        return core::makeError(core::ErrorCode::InvalidArgument, "no recordings selected");
    for (const ImportItem& item : plan.items) {
        if (item.sourceHandle == 0 || item.sourceName.empty() || item.sizeBytes == 0)
            return core::makeError(core::ErrorCode::InvalidArgument, "invalid import item");
    }
    auto free = storage_.freeSpaceBytes(projects_.project().paths().audioDirectory());
    if (!free) return free.error();
    if (free.value() < plan.totalBytes())
        return core::makeError(core::ErrorCode::OutOfSpace, "not enough local space for import");
    return core::success();
}

core::Status ImportService::run(const ImportPlan& plan, std::uint32_t jobId) {
    cancelRequested_.store(false, std::memory_order_release);
    core::Status validation = validate(plan);
    if (!validation) {
        postFinished(eventQueue_, jobId, validation);
        return validation;
    }

    std::vector<ImportedDeck> imported;
    std::vector<core::Path> completedFiles;
    const auto& paths = projects_.project().paths();
    std::uint64_t completedBytes = 0;
    auto fail = [&](core::Status failure) {
        for (const core::Path& completed : completedFiles) (void)storage_.remove(completed);
        postFinished(eventQueue_, jobId, failure);
        return failure;
    };

    for (std::size_t itemIndex = 0; itemIndex < plan.items.size(); ++itemIndex) {
        const ImportItem& item = plan.items[itemIndex];
        std::string name = storage::ProjectPaths::sanitiseName(item.sourceName);
        core::Path target = paths.audioFile(name);
        int suffix = 2;
        while (storage_.exists(target)) {
            const auto dot = name.find_last_of('.');
            const std::string base = dot == std::string::npos ? name : name.substr(0, dot);
            const std::string extension = dot == std::string::npos ? ".wav" : name.substr(dot);
            target = paths.audioFile(base + " " + std::to_string(suffix++) + extension);
        }
        const core::Path temporary = paths.temporaryFor(target);
        (void)storage_.remove(temporary);
        auto file = storage_.open(temporary, hal::OpenMode::Write);
        if (!file) {
            return fail(file.error());
        }
        std::optional<core::Error> writeError;
        std::uint64_t bytesWritten = 0;
        auto sink = [&](std::span<const std::byte> chunk) {
            std::size_t offset = 0;
            while (offset < chunk.size()) {
                auto written = file.value()->write(chunk.subspan(offset));
                if (!written || written.value() == 0) {
                    writeError = written ? core::makeError(core::ErrorCode::IoError, "short import write")
                                         : written.error();
                    return false;
                }
                offset += written.value();
                bytesWritten += written.value();
            }
            return !cancelRequested_.load(std::memory_order_acquire);
        };
        auto progress = [&](std::uint64_t done, std::uint64_t) {
            const std::uint64_t overall = completedBytes + done;
            const std::uint8_t percent = static_cast<std::uint8_t>(
                plan.totalBytes() == 0 ? 0 : std::min<std::uint64_t>(100, overall * 100 / plan.totalBytes()));
            eventQueue_.post(events::JobProgressEvent{events::JobKind::Import, jobId,
                                                       static_cast<std::uint32_t>(itemIndex),
                                                       static_cast<std::uint32_t>(plan.items.size()), percent});
            return !cancelRequested_.load(std::memory_order_acquire);
        };
        core::Status transfer = mtp_.readObject(item.sourceHandle, sink, progress);
        if (writeError) transfer = *writeError;
        if (cancelRequested_.load(std::memory_order_acquire))
            transfer = core::makeError(core::ErrorCode::Cancelled, "import cancelled");
        if (transfer && bytesWritten != item.sizeBytes)
            transfer = core::makeError(core::ErrorCode::Corrupt, "MTP object size changed during import");
        if (transfer) transfer = file.value()->flush();
        file.value().reset();
        if (transfer) transfer = storage_.rename(temporary, target);
        if (!transfer) {
            (void)storage_.remove(temporary);
            return fail(transfer);
        }

        auto metadata = audio::WavReader::readMetadata(storage_, target);
        if (!metadata) {
            (void)storage_.remove(target);
            return fail(metadata.error());
        }
        if (!metadata.value().isNativeFormat) {
            (void)storage_.remove(target);
            core::Status unsupported = core::makeError(
                core::ErrorCode::UnsupportedFormat, "import requires PCM 24-bit / 48 kHz WAV");
            return fail(unsupported);
        }
        imported.push_back({stem(target.filename().string()), target.filename().string(),
                            metadata.value().format.channels, metadata.value().frameCount});
        completedFiles.push_back(target);
        completedBytes += item.sizeBytes;
    }

    core::Status applied = projects_.execute(std::make_unique<AddImportedDecksCommand>(imported));
    if (!applied) {
        for (const core::Path& path : completedFiles) (void)storage_.remove(path);
    }
    postFinished(eventQueue_, jobId, applied);
    return applied;
}

void ImportService::requestCancel() noexcept {
    cancelRequested_.store(true, std::memory_order_release);
}

}  // namespace aw::project
