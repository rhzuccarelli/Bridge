#include "aw/project/ExportService.hpp"

#include "aw/audio/AudioBuffer.hpp"
#include "aw/audio/SampleConverter.hpp"
#include "aw/audio/WavReader.hpp"
#include "aw/audio/WavWriter.hpp"
#include "aw/core/Logger.hpp"
#include "aw/core/Thread.hpp"
#include "aw/storage/ProjectPaths.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <cmath>
#include <optional>
#include <span>
#include <thread>

namespace aw::project {
namespace {

/// Frames per render block. Small enough that a block's buffers (12 channels
/// of float, and their 24-bit bytes) stay in the P4's cache: converting in
/// 2048-frame blocks ran three times slower than in 512.
constexpr core::FrameCount kRenderBlockFrames = 512;

/// Source files are read in chunks of this size, whatever the block size:
/// one SD command each, through FatFsStorage's 64 KiB transfer buffer. Each
/// source reads its next chunk on a thread of its own while the render mixes.
constexpr std::size_t kSourceReadAheadBytes = 64 * 1024;

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

using Clock = std::chrono::steady_clock;

std::string milliseconds(Clock::duration duration) {
    return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(duration).count());
}

/// Mixes the deck one block at a time, as the upload asks for data.
///
/// One reader per source file, opened once for the whole render. Clips that
/// read the same file at the same position (every track of an imported deck)
/// share one read per block. Reopening and re-parsing the files for each block
/// and track took tens of thousands of opens for one minute of audio on the
/// P4's SD card.
class DeckRenderer {
public:
    DeckRenderer(hal::StorageInterface& storage, const Project& project, core::ChannelCount channels)
        : storage_{storage}, project_{project}, output_{channels, kRenderBlockFrames} {}

    /// Mixes `frames` frames from `blockStart` into `output()`.
    core::Status render(core::FrameIndex blockStart, core::FrameCount frames) {
        const Timeline& timeline = project_.timeline();
        output_.setFrameCount(frames);
        output_.clear();
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
                auto source = sourceFor(clip.sourceFile);
                if (!source) return source.error();
                const core::FrameIndex sourcePosition = clip.sourceRange.start +
                                                        overlapStart - clip.timelinePosition;
                const core::FrameCount overlapFrames = overlapEnd - overlapStart;
                if (auto status = source.value()->load(sourcePosition, overlapFrames, readTime); !status)
                    return status;
                const audio::AudioBuffer& sourceBlock = source.value()->block;
                const std::size_t destinationOffset = static_cast<std::size_t>(overlapStart - blockStart);
                const float gain = track.gain * clip.gain;
                for (std::size_t routeChannel = 0;
                     routeChannel < clip.destinationChannelCount; ++routeChannel) {
                    const std::size_t sourceIndex = clip.sourceChannel +
                        std::min<std::size_t>(routeChannel, clip.sourceChannelCount - 1U);
                    if (sourceIndex >= sourceBlock.channelCount()) continue;
                    const std::size_t pairChannel = clip.destinationChannel + routeChannel;
                    auto destination = output_.channel(static_cast<core::ChannelCount>(
                        trackIndex * 2U + pairChannel));
                    const auto samples = sourceBlock.channel(static_cast<core::ChannelCount>(sourceIndex));
                    for (std::size_t frame = 0; frame < samples.size(); ++frame)
                        destination[destinationOffset + frame] = std::clamp(
                            destination[destinationOffset + frame] +
                                samples[frame] * gain * panGain[pairChannel], -1.0F, 1.0F);
                }
            }
        }
        return core::success();
    }

    [[nodiscard]] const audio::AudioBuffer& output() const noexcept { return output_; }

    Clock::duration readTime{};

private:
    struct Source {
        std::string file;
        audio::WavReader reader;
        audio::AudioBuffer block;
        core::FrameIndex blockStart{-1};

        core::Status load(core::FrameIndex position, core::FrameCount frames, Clock::duration& time) {
            if (blockStart == position && block.frameCount() == frames) return core::success();
            if (reader.position() != position)
                if (auto status = reader.seek(position); !status) return status;
            block.setFrameCount(frames);
            const auto started = Clock::now();
            auto read = reader.read(block);
            time += Clock::now() - started;
            if (!read) return read.error();
            blockStart = position;
            return core::success();
        }
    };

    core::Result<Source*> sourceFor(const std::string& file) {
        for (const auto& source : sources_)
            if (source->file == file) return source.get();
        auto source = std::make_unique<Source>();
        source->file = file;
        if (auto status = source->reader.open(storage_, project_.paths().audioFile(file)); !status)
            return status.error();
        source->reader.setReadAhead(kSourceReadAheadBytes, true);
        source->block = audio::AudioBuffer{source->reader.metadata().format.channels, kRenderBlockFrames};
        sources_.push_back(std::move(source));
        return sources_.back().get();
    }

    hal::StorageInterface& storage_;
    const Project& project_;
    audio::AudioBuffer output_;
    std::vector<std::unique_ptr<Source>> sources_;
};


/// Renders the deck on a thread of its own, a few blocks ahead of the upload,
/// so reading and mixing overlap the USB transfers. Each block is converted
/// to the file's 24-bit PCM before it is queued.
class RenderAhead {
public:
    RenderAhead(hal::StorageInterface& storage, const Project& project,
                const core::AudioFormat& format, core::FrameCount totalFrames,
                std::vector<std::byte> header)
        : renderer_{storage, project, format.channels}, format_{format},
          totalFrames_{totalFrames}, current_{std::move(header)} {
        thread_ = core::threads::start(core::ThreadRole::Compute, [this] { run(); });
    }

    ~RenderAhead() { stop(); }

    RenderAhead(const RenderAhead&) = delete;
    RenderAhead& operator=(const RenderAhead&) = delete;

    /// The upload's data source: the header, then the rendered blocks in order.
    core::Result<std::size_t> read(std::span<std::byte> buffer) {
        if (offset_ == current_.size()) {
            std::unique_lock lock{mutex_};
            if (!current_.empty()) spare_.push_back(std::move(current_));
            current_.clear();
            changed_.notify_all();
            const auto started = Clock::now();
            changed_.wait(lock, [this] { return !ready_.empty() || finished_; });
            waitTime += Clock::now() - started;
            if (ready_.empty()) {
                if (!failure_) return failure_.error();
                return std::size_t{0};
            }
            current_ = std::move(ready_.front());
            ready_.pop_front();
            offset_ = 0;
            changed_.notify_all();
        }
        const std::size_t n = std::min(buffer.size(), current_.size() - offset_);
        std::copy_n(current_.begin() + static_cast<std::ptrdiff_t>(offset_), n, buffer.begin());
        offset_ += n;
        return n;
    }

    /// Ends the render thread, finished or not. The timings are valid after it.
    void stop() {
        {
            std::lock_guard lock{mutex_};
            stopping_ = true;
        }
        changed_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    [[nodiscard]] Clock::duration readTime() const noexcept { return renderer_.readTime; }
    [[nodiscard]] Clock::duration renderTime() const noexcept { return renderTime_; }
    Clock::duration waitTime{};

private:
    /// Blocks rendered ahead, 288 KiB at 12 channels.
    static constexpr std::size_t kAhead = 16;

    void run() {
        std::vector<std::span<const float>> planes(format_.channels);
        for (core::FrameIndex next = 0; next < totalFrames_;) {
            std::vector<std::byte> block;
            {
                std::unique_lock lock{mutex_};
                changed_.wait(lock, [this] { return stopping_ || ready_.size() < kAhead; });
                if (stopping_) break;
                if (!spare_.empty()) {
                    block = std::move(spare_.back());
                    spare_.pop_back();
                }
            }
            const auto started = Clock::now();
            const core::FrameCount frames = std::min(kRenderBlockFrames, totalFrames_ - next);
            core::Status status = renderer_.render(next, frames);
            if (status) {
                for (core::ChannelCount channel = 0; channel < format_.channels; ++channel)
                    planes[channel] = renderer_.output().channel(channel);
                block.resize(static_cast<std::size_t>(frames) * format_.bytesPerFrame());
                status = audio::convert::fromFloat(planes, format_, block, frames);
            }
            renderTime_ += Clock::now() - started;
            std::lock_guard lock{mutex_};
            if (!status) {
                failure_ = status;
                break;
            }
            ready_.push_back(std::move(block));
            next += frames;
            changed_.notify_all();
        }
        std::lock_guard lock{mutex_};
        finished_ = true;
        changed_.notify_all();
    }

    DeckRenderer renderer_;
    const core::AudioFormat format_;
    const core::FrameCount totalFrames_;
    Clock::duration renderTime_{};

    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<std::vector<std::byte>> ready_;
    std::vector<std::vector<std::byte>> spare_;
    core::Status failure_ = core::success();
    bool finished_{false};
    bool stopping_{false};

    // Consumer side, touched only by `read()`.
    std::vector<std::byte> current_;
    std::size_t offset_{0};

    std::thread thread_;
};

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
    const std::vector<std::byte> header = audio::WavWriter::header(format, acid, totalFrames);

    std::string notDirect;
    const std::optional<std::string> direct =
        passthroughSource(timeline, channels, totalFrames, storage_, project.paths(), notDirect);
    std::unique_ptr<hal::FileHandle> directFile;
    std::uint64_t uploadBytes = header.size() +
                                static_cast<std::uint64_t>(totalFrames) * format.bytesPerFrame();
    if (direct) {
        auto opened = storage_.open(project.paths().audioFile(*direct), hal::OpenMode::Read);
        auto size = opened ? opened.value()->size() : core::Result<std::uint64_t>{opened.error()};
        if (!size) {
            postFinished(eventQueue_, jobId, size.error());
            return size.error();
        }
        directFile = std::move(opened).value();
        uploadBytes = size.value();
        core::log::info("export", "deck plays " + *direct + " unchanged; sending it without rendering");
    } else {
        core::log::info("export", "rendering the deck: " + notDirect);
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

    if (direct) {
        return upload(plan, targetName, uploadBytes, jobId,
                      [&](std::span<std::byte> buffer) -> core::Result<std::size_t> {
                          return directFile->read(buffer);
                      });
    }

    // The render goes straight into the upload: the header, then each block
    // converted to 24-bit, rendered on a thread of its own while the previous
    // blocks are sent. Nothing is written to the card.
    core::log::info("export", "rendering " + std::to_string(totalFrames) + " frames, " +
                                  std::to_string(channels) + " channels into the upload");
    const auto started = Clock::now();
    RenderAhead render{storage_, project, format, totalFrames, header};
    core::Status status = upload(plan, targetName, uploadBytes, jobId,
                                 [&](std::span<std::byte> buffer) { return render.read(buffer); });
    render.stop();
    const auto total = Clock::now() - started;
    core::log::info("export", "export took " + milliseconds(total) + " ms: reading sources " +
                                  milliseconds(render.readTime()) + " ms, mixing and converting " +
                                  milliseconds(render.renderTime() - render.readTime()) +
                                  " ms, upload waited " + milliseconds(render.waitTime) +
                                  " ms for the render");
    return status;
}

core::Status ExportService::upload(const ExportPlan& plan, std::string targetName,
                                   std::uint64_t bytes, std::uint32_t jobId,
                                   const DataSource& produce) {
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

    if (status) {
        // MTP takes exactly `bytes` once SendObjectInfo has announced them, and
        // a data phase cannot be cut short. After a failure or a cancel the
        // rest is zeros, and the object is deleted once it is complete.
        std::uint64_t sent = 0;
        bool failed = false;
        auto source = [&](std::span<std::byte> buffer) -> std::size_t {
            buffer = buffer.first(static_cast<std::size_t>(
                std::min<std::uint64_t>(buffer.size(), bytes - sent)));
            if (buffer.empty()) return 0;
            std::size_t n = 0;
            if (!failed && !cancelRequested_.load(std::memory_order_acquire)) {
                auto produced = produce(buffer);
                if (!produced) status = produced.error();
                else if (produced.value() == 0)
                    status = core::makeError(core::ErrorCode::IoError, "export data ended early");
                else n = produced.value();
                failed = !status;
            }
            if (n == 0) {
                std::fill(buffer.begin(), buffer.end(), std::byte{0});
                n = buffer.size();
            }
            sent += n;
            return n;
        };
        auto progress = [&](std::uint64_t done, std::uint64_t total) {
            const auto percent = static_cast<std::uint8_t>(total == 0 ? 0 : done * 100 / total);
            eventQueue_.post(events::JobProgressEvent{events::JobKind::Export, jobId, 0, 1, percent});
            return !cancelRequested_.load(std::memory_order_acquire);
        };
        core::log::info("export", "sending " + std::to_string(bytes >> 20) + " MiB to the TP-7");
        auto uploaded = mtp_.writeObject(plan.targetStorage, plan.targetFolder, targetName,
                                         bytes, source, progress);
        if (!uploaded) {
            if (status) status = uploaded.error();
        } else if (!status) {
            (void)mtp_.deleteObject(uploaded.value());
        }
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
