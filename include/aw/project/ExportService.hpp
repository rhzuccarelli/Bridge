#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "aw/audio/WavWriter.hpp"
#include "aw/core/AudioFormat.hpp"
#include "aw/core/Result.hpp"
#include "aw/core/Types.hpp"
#include "aw/events/EventQueue.hpp"
#include "aw/hal/MTPInterface.hpp"
#include "aw/hal/StorageInterface.hpp"
#include "aw/project/ProjectManager.hpp"

namespace aw::project {

/// What to export and where it goes on the device.
struct ExportItem {
    core::TrackId trackId{core::kInvalidTrackId};
    std::string targetName;  ///< File name to create on the TP-7.
};

enum class ExportConflictPolicy : std::uint8_t {
    Fail,       ///< Default. Refuse and tell the user.
    Rename,     ///< Append " 2", " 3", ...
    Overwrite,  ///< Delete then write. Only after explicit confirmation.
};

struct ExportPlan {
    /// One active deck is exported as one TP-7 WAV containing 1-6 stereo pairs.
    std::string targetName;
    std::vector<ExportItem> items;
    hal::MtpStorageId targetStorage{0};
    hal::MtpObjectHandle targetFolder{hal::kMtpRootHandle};
    ExportConflictPolicy conflictPolicy{ExportConflictPolicy::Fail};
};

/// Renders tracks and writes them back to the TP-7 over MTP.
///
/// A track with edits is not a file on disk - it is a clip list - so the deck
/// is rendered block by block straight into the MTP upload. MTP needs the exact
/// byte count up front; a 24-bit WAV's size follows from the deck's length, so
/// nothing is written to local storage first. An unedited deck is sent as its
/// source file.
///
/// Never overwrites without `ExportConflictPolicy::Overwrite`, which the UI only
/// sets after a confirmation screen.
class ExportService {
public:
    ExportService(hal::StorageInterface& storage,
                  hal::MTPInterface& mtp,
                  ProjectManager& projects,
                  events::EventQueue& eventQueue);

    ExportService(const ExportService&) = delete;
    ExportService& operator=(const ExportService&) = delete;

    /// Checks device free space and reports name collisions without writing.
    [[nodiscard]] core::Result<std::vector<std::string>> findConflicts(
        const ExportPlan& plan) const;

    /// Blocking: renders then uploads.  Progress covers both phases.
    [[nodiscard]] core::Status run(const ExportPlan& plan, std::uint32_t jobId);

    void requestCancel() noexcept;

private:
    /// Fills `buffer` with the export's next bytes; 0 means the data ended.
    using DataSource = std::function<core::Result<std::size_t>(std::span<std::byte> buffer)>;

    /// Sends `bytes` bytes from `produce` to the TP-7 as `targetName` and posts
    /// the finished event.
    core::Status upload(const ExportPlan& plan, std::string targetName, std::uint64_t bytes,
                        std::uint32_t jobId, const DataSource& produce);

    hal::StorageInterface& storage_;
    hal::MTPInterface& mtp_;
    ProjectManager& projects_;
    events::EventQueue& eventQueue_;
    std::atomic<bool> cancelRequested_{false};
};

}  // namespace aw::project
