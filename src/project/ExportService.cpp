#include "aw/project/ExportService.hpp"

// Phase 8 implements render-then-upload.

namespace aw::project {

ExportService::ExportService(hal::StorageInterface& storage,
                             hal::MTPInterface& mtp,
                             ProjectManager& projects,
                             events::EventQueue& eventQueue)
    : storage_{storage}, mtp_{mtp}, projects_{projects}, eventQueue_{eventQueue} {}

core::Result<std::vector<std::string>> ExportService::findConflicts(
    const ExportPlan& /*plan*/) const {
    return core::notImplemented("ExportService::findConflicts (Phase 8)");
}

core::Status ExportService::run(const ExportPlan& /*plan*/, std::uint32_t /*jobId*/) {
    (void)storage_;
    (void)mtp_;
    (void)projects_;
    (void)eventQueue_;
    return core::notImplemented("ExportService::run (Phase 8)");
}

void ExportService::requestCancel() noexcept {
    cancelRequested_.store(true, std::memory_order_release);
}

}  // namespace aw::project
