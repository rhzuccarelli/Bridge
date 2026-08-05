#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "aw/core/Result.hpp"
#include "aw/core/Types.hpp"
#include "aw/events/EventQueue.hpp"
#include "aw/hal/MTPInterface.hpp"
#include "aw/hal/StorageInterface.hpp"
#include "aw/project/ProjectManager.hpp"

namespace aw::project {

/// One file the user selected for import.
struct ImportItem {
    hal::MtpObjectHandle sourceHandle{0};
    std::string sourceName;
    std::uint64_t sizeBytes{0};

    /// Track to place it on.  `kInvalidTrackId` means "next free track".
    core::TrackId targetTrack{core::kInvalidTrackId};
};

struct ImportPlan {
    std::vector<ImportItem> items;

    [[nodiscard]] std::uint64_t totalBytes() const noexcept;
};

/// Copies selected recordings off the TP-7 into the open project.
///
/// Phase 8 implements this.  The rules it encodes are product decisions, not
/// implementation details, so they are written down here now:
///
///  - Nothing is ever imported automatically.  A device appearing produces a
///    notification, never a transfer.  Auto-sync on a device that holds someone's
///    only copy of a take is how you lose takes.
///  - The MTP object is streamed straight to a temp file in the project's
///    `audio/` directory, then renamed on success.  A cancelled or failed
///    import leaves no half file behind.
///  - The source on the TP-7 is never modified or deleted.  Import is a copy.
///  - Free space is checked before the first byte moves.
///
/// Runs on the I/O worker thread; reports through `events::JobProgressEvent`.
class ImportService {
public:
    ImportService(hal::StorageInterface& storage,
                  hal::MTPInterface& mtp,
                  ProjectManager& projects,
                  events::EventQueue& eventQueue);

    ImportService(const ImportService&) = delete;
    ImportService& operator=(const ImportService&) = delete;

    /// Validates the plan against free space and the project's track capacity.
    [[nodiscard]] core::Status validate(const ImportPlan& plan) const;

    /// Blocking.  Copies every item, then applies one command per imported file
    /// so the whole import is a single undo step.
    [[nodiscard]] core::Status run(const ImportPlan& plan, std::uint32_t jobId);

    /// Asks a running import to stop at the next chunk boundary.
    void requestCancel() noexcept;

private:
    hal::StorageInterface& storage_;
    hal::MTPInterface& mtp_;
    ProjectManager& projects_;
    events::EventQueue& eventQueue_;
    std::atomic<bool> cancelRequested_{false};
};

}  // namespace aw::project
