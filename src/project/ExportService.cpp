#include "aw/project/ExportService.hpp"

#include "aw/audio/AudioBuffer.hpp"
#include "aw/audio/WavReader.hpp"
#include "aw/audio/WavWriter.hpp"
#include "aw/storage/ProjectPaths.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

namespace aw::project {
namespace {

constexpr core::FrameCount kRenderBlockFrames = 512;

void postFinished(events::EventQueue& queue, std::uint32_t jobId, const core::Status& status) {
    queue.post(events::JobFinishedEvent{events::JobKind::Export, jobId,
                                        static_cast<bool>(status),
                                        status ? core::ErrorCode::Unknown : status.error().code()});
}

std::string exportName(const ExportPlan& plan, const Project& project) {
    std::string name = plan.targetName.empty() ? project.name() + ".wav" : plan.targetName;
    if (name.size() < 4 || name.substr(name.size() - 4) != ".wav") name += ".wav";
    return storage::ProjectPaths::sanitiseName(name);
}

core::ChannelCount outputChannels(const Timeline& timeline) {
    std::size_t highest = 0;
    for (std::size_t index = 0; index < timeline.tracks.size() && index < kTrackCount; ++index)
        if (!timeline.tracks[index].clips.empty()) highest = index + 1;
    return static_cast<core::ChannelCount>(std::max<std::size_t>(1, highest) * 2U);
}

}  // namespace

ExportService::ExportService(hal::StorageInterface& storage, hal::MTPInterface& mtp,
                             ProjectManager& projects, events::EventQueue& eventQueue)
    : storage_{storage}, mtp_{mtp}, projects_{projects}, eventQueue_{eventQueue} {}

core::Result<std::vector<std::string>> ExportService::findConflicts(const ExportPlan& plan) const {
    if (!mtp_.hasSession())
        return core::makeError(core::ErrorCode::DeviceNotConnected, "TP-7 MTP session is closed");
    if (!projects_.hasProject())
        return core::makeError(core::ErrorCode::InvalidState, "no project open");
    auto objects = mtp_.listObjects(plan.targetStorage, plan.targetFolder);
    if (!objects) return objects.error();
    const std::string name = exportName(plan, projects_.project());
    std::vector<std::string> conflicts;
    for (const auto& object : objects.value())
        if (!object.isFolder && object.name == name) conflicts.push_back(name);
    return conflicts;
}

core::Status ExportService::run(const ExportPlan& plan, std::uint32_t jobId) {
    cancelRequested_.store(false, std::memory_order_release);
    if (!projects_.hasProject()) {
        core::Status error = core::makeError(core::ErrorCode::InvalidState, "no project open");
        postFinished(eventQueue_, jobId, error);
        return error;
    }
    if (!mtp_.hasSession()) {
        core::Status error = core::makeError(core::ErrorCode::DeviceNotConnected, "TP-7 disconnected");
        postFinished(eventQueue_, jobId, error);
        return error;
    }

    const Project& project = projects_.project();
    const Timeline& timeline = project.timeline();
    const core::FrameCount totalFrames = timeline.lengthFrames();
    if (totalFrames <= 0) {
        core::Status error = core::makeError(core::ErrorCode::InvalidState, "active deck is empty");
        postFinished(eventQueue_, jobId, error);
        return error;
    }
    std::string targetName = exportName(plan, project);
    const core::ChannelCount channels = outputChannels(timeline);
    const core::AudioFormat format{48000, channels, core::SampleFormat::Int24};
    const std::uint64_t estimatedBytes = 44U + static_cast<std::uint64_t>(totalFrames) *
                                                  format.bytesPerFrame();

    auto localFree = storage_.freeSpaceBytes(project.paths().editsDirectory());
    if (!localFree || localFree.value() < estimatedBytes) {
        core::Status error = localFree ? core::makeError(core::ErrorCode::OutOfSpace, "no render space")
                                       : localFree.error();
        postFinished(eventQueue_, jobId, error);
        return error;
    }
    auto storages = mtp_.storages();
    if (!storages) {
        postFinished(eventQueue_, jobId, storages.error());
        return storages.error();
    }
    const auto storageInfo = std::find_if(storages.value().begin(), storages.value().end(),
        [&](const hal::MtpStorageInfo& info) { return info.id == plan.targetStorage; });
    if (storageInfo == storages.value().end() || storageInfo->freeBytes < estimatedBytes) {
        core::Status error = core::makeError(core::ErrorCode::OutOfSpace, "not enough TP-7 space");
        postFinished(eventQueue_, jobId, error);
        return error;
    }

    const core::Path renderPath = project.paths().editsDirectory() / targetName;
    (void)storage_.remove(renderPath);
    audio::WavWriter writer;
    core::Status status = writer.create(storage_, renderPath, format);
    audio::AudioBuffer output{channels, kRenderBlockFrames};

    for (core::FrameIndex blockStart = 0; status && blockStart < totalFrames;
         blockStart += kRenderBlockFrames) {
        if (cancelRequested_.load(std::memory_order_acquire)) {
            status = core::makeError(core::ErrorCode::Cancelled, "export cancelled");
            break;
        }
        const core::FrameCount frames = std::min(kRenderBlockFrames, totalFrames - blockStart);
        output.setFrameCount(frames);
        output.clear();
        for (std::size_t trackIndex = 0;
             trackIndex < timeline.tracks.size() && trackIndex < kTrackCount; ++trackIndex) {
            const Track& track = timeline.tracks[trackIndex];
            if (track.muted || (timeline.hasSolo() && !track.soloed)) continue;
            const float pan = std::clamp(track.pan, -1.0F, 1.0F);
            const float panGain[2]{pan <= 0.0F ? 1.0F : 1.0F - pan,
                                   pan >= 0.0F ? 1.0F : 1.0F + pan};
            for (const Clip& clip : track.clips) {
                const core::FrameIndex overlapStart = std::max(blockStart, clip.timelinePosition);
                const core::FrameIndex overlapEnd = std::min(blockStart + frames, clip.timelineEnd());
                if (overlapStart >= overlapEnd) continue;
                auto metadata = audio::WavReader::readMetadata(storage_, project.paths().audioFile(clip.sourceFile));
                if (!metadata) { status = metadata.error(); break; }
                audio::WavReader reader;
                status = reader.open(storage_, project.paths().audioFile(clip.sourceFile));
                if (!status) break;
                const core::FrameIndex sourcePosition = clip.sourceRange.start +
                                                        overlapStart - clip.timelinePosition;
                status = reader.seek(sourcePosition);
                if (!status) break;
                const core::FrameCount overlapFrames = overlapEnd - overlapStart;
                audio::AudioBuffer source{metadata.value().format.channels, overlapFrames};
                auto read = reader.read(source);
                if (!read) { status = read.error(); break; }
                const std::size_t destinationOffset = static_cast<std::size_t>(overlapStart - blockStart);
                const float gain = track.gain * clip.gain;
                for (std::size_t pairChannel = 0; pairChannel < 2; ++pairChannel) {
                    const std::size_t sourceIndex = clip.sourceChannel +
                        std::min<std::size_t>(pairChannel, clip.sourceChannelCount - 1U);
                    if (sourceIndex >= source.channelCount()) continue;
                    auto destination = output.channel(static_cast<core::ChannelCount>(trackIndex * 2U + pairChannel));
                    const auto samples = source.channel(static_cast<core::ChannelCount>(sourceIndex));
                    for (std::size_t frame = 0; frame < samples.size(); ++frame)
                        destination[destinationOffset + frame] = std::clamp(
                            destination[destinationOffset + frame] +
                                samples[frame] * gain * panGain[pairChannel], -1.0F, 1.0F);
                }
            }
            if (!status) break;
        }
        if (status) status = writer.write(output);
        const auto percent = static_cast<std::uint8_t>(std::min<core::FrameIndex>(
            50, (blockStart + frames) * 50 / totalFrames));
        eventQueue_.post(events::JobProgressEvent{events::JobKind::Export, jobId, 0, 2, percent});
    }
    if (status) status = writer.close();
    else (void)writer.close();
    if (!status) {
        (void)storage_.remove(renderPath);
        postFinished(eventQueue_, jobId, status);
        return status;
    }

    auto conflicts = mtp_.listObjects(plan.targetStorage, plan.targetFolder);
    if (!conflicts) status = conflicts.error();
    hal::MtpObjectHandle conflictHandle = 0;
    if (conflicts) {
        for (const auto& object : conflicts.value())
            if (!object.isFolder && object.name == targetName) conflictHandle = object.handle;
        if (conflictHandle != 0 && plan.conflictPolicy == ExportConflictPolicy::Fail)
            status = core::makeError(core::ErrorCode::AlreadyExists, targetName);
        if (conflictHandle != 0 && plan.conflictPolicy == ExportConflictPolicy::Overwrite)
            status = mtp_.deleteObject(conflictHandle);
        if (conflictHandle != 0 && plan.conflictPolicy == ExportConflictPolicy::Rename) {
            const auto dot = targetName.find_last_of('.');
            const std::string base = dot == std::string::npos ? targetName : targetName.substr(0, dot);
            const std::string extension = dot == std::string::npos ? ".wav" : targetName.substr(dot);
            for (int suffix = 2;; ++suffix) {
                const std::string candidate = base + " " + std::to_string(suffix) + extension;
                const bool taken = std::any_of(conflicts.value().begin(), conflicts.value().end(),
                    [&](const hal::MtpObjectInfo& object) {
                        return !object.isFolder && object.name == candidate;
                    });
                if (!taken) { targetName = candidate; break; }
            }
        }
    }

    auto file = status ? storage_.open(renderPath, hal::OpenMode::Read)
                       : core::Result<std::unique_ptr<hal::FileHandle>>{status.error()};
    if (status && !file) status = file.error();
    if (status) {
        auto source = [&](std::span<std::byte> buffer) -> std::size_t {
            if (cancelRequested_.load(std::memory_order_acquire)) return 0;
            auto read = file.value()->read(buffer);
            if (!read) { status = read.error(); return 0; }
            return read.value();
        };
        auto progress = [&](std::uint64_t done, std::uint64_t total) {
            const auto percent = static_cast<std::uint8_t>(50 + (total == 0 ? 0 : done * 50 / total));
            eventQueue_.post(events::JobProgressEvent{events::JobKind::Export, jobId, 1, 2, percent});
            return !cancelRequested_.load(std::memory_order_acquire);
        };
        auto uploaded = mtp_.writeObject(plan.targetStorage, plan.targetFolder, targetName,
                                         estimatedBytes, source, progress);
        if (!uploaded) status = uploaded.error();
    }
    if (cancelRequested_.load(std::memory_order_acquire))
        status = core::makeError(core::ErrorCode::Cancelled, "export cancelled");
    postFinished(eventQueue_, jobId, status);
    return status;
}

void ExportService::requestCancel() noexcept {
    cancelRequested_.store(true, std::memory_order_release);
}

}  // namespace aw::project
