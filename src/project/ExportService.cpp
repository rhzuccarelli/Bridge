#include "aw/project/ExportService.hpp"

#include "aw/audio/AudioBuffer.hpp"
#include "aw/audio/WavReader.hpp"
#include "aw/audio/WavWriter.hpp"
#include "aw/core/Logger.hpp"
#include "aw/storage/ProjectPaths.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include <cmath>
#include <optional>

namespace aw::project {
namespace {

/// Frames per render block. 8192 was slower than 512 on the P4: the buffers
/// (12 channels of float) no longer fit the cache.
constexpr core::FrameCount kRenderBlockFrames = 2048;

void postFinished(events::EventQueue& queue, std::uint32_t jobId, const core::Status& status) {
    // The screen can show only the error code; the log keeps the reason.
    if (status) core::log::info("export", "export completed");
    else core::log::warning("export", "export failed: " + status.error().describe());
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

/// The source file when the deck plays one WAV straight through, unchanged:
/// every track up to the last used one holds exactly one clip of it, routing
/// pair 2i/2i+1 to track i over the whole file at unity gain, centre pan,
/// without fades, nothing muted or soloed, and the file is already 48 kHz
/// 24-bit with the export's channel count. A freshly imported TP-7 take is
/// such a deck; rendering it would reproduce the file, so it is sent as is.
std::optional<std::string> passthroughSource(const Timeline& timeline, core::ChannelCount channels,
                                             core::FrameCount totalFrames,
                                             hal::StorageInterface& storage,
                                             const storage::ProjectPaths& paths,
                                             std::string& reason) {
    const auto no = [&](std::string why) {
        reason = std::move(why);
        return std::nullopt;
    };
    if (timeline.hasSolo()) return no("a track is soloed");
    const std::size_t usedTracks = channels / 2U;
    std::optional<std::string> file;
    for (std::size_t index = 0; index < timeline.tracks.size() && index < kTrackCount; ++index) {
        const Track& track = timeline.tracks[index];
        const std::string name = "track " + std::to_string(index + 1);
        if (index >= usedTracks) {
            if (!track.clips.empty()) return no(name + " has clips beyond the output");
            continue;
        }
        if (track.muted) return no(name + " is muted");
        if (track.gain != 1.0F) return no(name + " gain is " + std::to_string(track.gain));
        if (track.pan != 0.0F) return no(name + " pan is " + std::to_string(track.pan));
        if (track.clips.size() != 1) return no(name + " has " + std::to_string(track.clips.size()) + " clips");
        const Clip& clip = track.clips.front();
        if (clip.gain != 1.0F) return no(name + " clip gain is " + std::to_string(clip.gain));
        if (clip.fadeInFrames != 0 || clip.fadeOutFrames != 0) return no(name + " clip has fades");
        if (clip.timelinePosition != 0 || clip.sourceRange.start != 0 ||
            clip.sourceRange.length != totalFrames)
            return no(name + " clip is moved or trimmed");
        if (clip.sourceChannel != static_cast<core::ChannelCount>(index * 2U) ||
            clip.sourceChannelCount != 2 || clip.destinationChannel != 0 ||
            clip.destinationChannelCount != 2)
            return no(name + " clip routes channels " + std::to_string(clip.sourceChannel) + "+" +
                      std::to_string(clip.sourceChannelCount) + " to " +
                      std::to_string(clip.destinationChannel) + "+" +
                      std::to_string(clip.destinationChannelCount));
        if (file && *file != clip.sourceFile) return no("tracks play different files");
        file = clip.sourceFile;
    }
    if (!file) return no("no clips");
    auto metadata = audio::WavReader::readMetadata(storage, paths.audioFile(*file));
    if (!metadata) return no("cannot read " + *file);
    if (!metadata.value().isNativeFormat || metadata.value().format.channels != channels ||
        metadata.value().frameCount != totalFrames)
        return no(*file + " is not " + std::to_string(channels) + "-channel 48 kHz 24-bit of the deck's length");
    return file;
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
    const std::string targetName = exportName(plan, project);
    const core::ChannelCount channels = outputChannels(timeline);
    const core::AudioFormat format{48000, channels, core::SampleFormat::Int24};
    std::optional<audio::AcidMetadata> acid;
    for (const Track& track : timeline.tracks) {
        for (const Clip& clip : track.clips) {
            auto sourceMetadata = audio::WavReader::readMetadata(
                storage_, project.paths().audioFile(clip.sourceFile));
            if (sourceMetadata && sourceMetadata.value().acid) {
                acid = sourceMetadata.value().acid;
                break;
            }
        }
        if (acid) break;
    }
    const std::uint64_t estimatedBytes = (acid ? 76U : 44U) +
                                          static_cast<std::uint64_t>(totalFrames) *
                                                  format.bytesPerFrame();

    std::string notDirect;
    const std::optional<std::string> direct =
        passthroughSource(timeline, channels, totalFrames, storage_, project.paths(), notDirect);
    if (!direct) core::log::info("export", "rendering the deck: " + notDirect);
    std::uint64_t uploadBytes = estimatedBytes;
    if (direct) {
        auto sourceFile = storage_.open(project.paths().audioFile(*direct), hal::OpenMode::Read);
        auto sourceSize = sourceFile ? sourceFile.value()->size()
                                     : core::Result<std::uint64_t>{sourceFile.error()};
        if (!sourceSize) {
            postFinished(eventQueue_, jobId, sourceSize.error());
            return sourceSize.error();
        }
        uploadBytes = sourceSize.value();
        core::log::info("export", "deck plays " + *direct + " unchanged; sending it without rendering");
    }

    auto localFree = storage_.freeSpaceBytes(project.paths().editsDirectory());
    if (!direct && (!localFree || localFree.value() < estimatedBytes)) {
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
    if (storageInfo == storages.value().end() || storageInfo->freeBytes < uploadBytes) {
        core::Status error = core::makeError(core::ErrorCode::OutOfSpace, "not enough TP-7 space");
        postFinished(eventQueue_, jobId, error);
        return error;
    }

    const core::Path renderPath = direct ? project.paths().audioFile(*direct)
                                         : project.paths().editsDirectory() / targetName;
    core::Status status = direct ? core::success() : render(plan, renderPath, format, acid, jobId);
    if (!status) {
        postFinished(eventQueue_, jobId, status);
        return status;
    }
    return upload(plan, renderPath, targetName, uploadBytes, jobId);
}

core::Status ExportService::render(const ExportPlan& /*plan*/, const core::Path& renderPath,
                                   const core::AudioFormat& format,
                                   const std::optional<audio::AcidMetadata>& acid, std::uint32_t jobId) {
    const Project& project = projects_.project();
    const Timeline& timeline = project.timeline();
    const core::FrameCount totalFrames = timeline.lengthFrames();
    const core::ChannelCount channels = format.channels;
    const std::string targetName = renderPath.filename().string();
    (void)storage_.remove(renderPath);
    audio::WavWriter writer;
    core::Status status = writer.create(storage_, renderPath, format, acid);
    audio::AudioBuffer output{channels, kRenderBlockFrames};

    // One reader per source file, opened once for the whole render. Clips that
    // read the same file at the same position (every track of an imported
    // deck) share one read per block. Reopening and re-parsing the files for
    // each block and track took tens of thousands of opens for one minute of
    // audio on the P4's SD card.
    struct Source {
        std::string file;
        audio::WavReader reader;
        audio::AudioBuffer block;
        core::FrameIndex blockStart{-1};
    };
    std::vector<std::unique_ptr<Source>> sources;
    using clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;
    const auto started = clock::now();
    int loggedTenth = 0;
    clock::duration readTime{};
    clock::duration writeTime{};
    core::log::info("export", "rendering " + std::to_string(totalFrames) + " frames, " +
                                  std::to_string(channels) + " channels to " + targetName);
    const auto sourceFor = [&](const std::string& file) -> Source* {
        for (const auto& source : sources)
            if (source->file == file) return source.get();
        auto source = std::make_unique<Source>();
        source->file = file;
        status = source->reader.open(storage_, project.paths().audioFile(file));
        if (!status) return nullptr;
        source->block = audio::AudioBuffer{source->reader.metadata().format.channels, kRenderBlockFrames};
        sources.push_back(std::move(source));
        return sources.back().get();
    };

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
                Source* source = sourceFor(clip.sourceFile);
                if (source == nullptr) break;
                const core::FrameIndex sourcePosition = clip.sourceRange.start +
                                                        overlapStart - clip.timelinePosition;
                const core::FrameCount overlapFrames = overlapEnd - overlapStart;
                if (source->blockStart != sourcePosition || source->block.frameCount() != overlapFrames) {
                    if (source->reader.position() != sourcePosition) {
                        status = source->reader.seek(sourcePosition);
                        if (!status) break;
                    }
                    source->block.setFrameCount(overlapFrames);
                    const auto readStarted = clock::now();
                    auto read = source->reader.read(source->block);
                    readTime += clock::now() - readStarted;
                    if (!read) { status = read.error(); break; }
                    source->blockStart = sourcePosition;
                }
                const audio::AudioBuffer& sourceBlock = source->block;
                const std::size_t destinationOffset = static_cast<std::size_t>(overlapStart - blockStart);
                const float gain = track.gain * clip.gain;
                for (std::size_t routeChannel = 0;
                     routeChannel < clip.destinationChannelCount; ++routeChannel) {
                    const std::size_t sourceIndex = clip.sourceChannel +
                        std::min<std::size_t>(routeChannel, clip.sourceChannelCount - 1U);
                    if (sourceIndex >= sourceBlock.channelCount()) continue;
                    const std::size_t pairChannel = clip.destinationChannel + routeChannel;
                    auto destination = output.channel(static_cast<core::ChannelCount>(
                        trackIndex * 2U + pairChannel));
                    const auto samples = sourceBlock.channel(static_cast<core::ChannelCount>(sourceIndex));
                    for (std::size_t frame = 0; frame < samples.size(); ++frame)
                        destination[destinationOffset + frame] = std::clamp(
                            destination[destinationOffset + frame] +
                                samples[frame] * gain * panGain[pairChannel], -1.0F, 1.0F);
                }
            }
            if (!status) break;
        }
        const auto writeStarted = clock::now();
        if (status) status = writer.write(output);
        writeTime += clock::now() - writeStarted;
        const auto percent = static_cast<std::uint8_t>(std::min<core::FrameIndex>(
            50, (blockStart + frames) * 50 / totalFrames));
        eventQueue_.post(events::JobProgressEvent{events::JobKind::Export, jobId, 0, 2, percent});
        if (percent / 5 != loggedTenth) {
            loggedTenth = percent / 5;
            core::log::info("export", "rendered " + std::to_string(percent * 2) + " % (" +
                                          std::to_string((clock::now() - started) / 1ms) + " ms)");
        }
    }
    sources.clear();  // Closes the source files before the upload opens the render.
    const auto ms = [](clock::duration d) {
        return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(d).count());
    };
    const auto total = clock::now() - started;
    core::log::info("export", "render took " + ms(total) + " ms: reading sources " + ms(readTime) +
                                  " ms, writing " + ms(writeTime) + " ms, mixing " +
                                  ms(total - readTime - writeTime) + " ms");
    if (status) status = writer.close();
    else (void)writer.close();
    if (!status) (void)storage_.remove(renderPath);
    return status;
}

core::Status ExportService::upload(const ExportPlan& plan, const core::Path& sourcePath,
                                   std::string targetName, std::uint64_t bytes, std::uint32_t jobId) {
    core::Status status = core::success();
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

    auto file = status ? storage_.open(sourcePath, hal::OpenMode::Read)
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
        const auto uploadStarted = std::chrono::steady_clock::now();
        core::log::info("export", "sending " + std::to_string(bytes >> 20) + " MiB to the TP-7");
        auto uploaded = mtp_.writeObject(plan.targetStorage, plan.targetFolder, targetName,
                                         bytes, source, progress);
        core::log::info("export", "send returned after " +
                                      std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now() - uploadStarted).count()) + " ms");
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
