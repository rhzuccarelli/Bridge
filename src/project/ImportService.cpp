#include "aw/project/ImportService.hpp"

// Phase 8 implements the transfer.

namespace aw::project {

std::uint64_t ImportPlan::totalBytes() const noexcept {
    std::uint64_t total = 0;
    for (const ImportItem& item : items) {
        total += item.sizeBytes;
    }
    return total;
}

ImportService::ImportService(hal::StorageInterface& storage,
                             hal::MTPInterface& mtp,
                             ProjectManager& projects,
                             events::EventQueue& eventQueue)
    : storage_{storage}, mtp_{mtp}, projects_{projects}, eventQueue_{eventQueue} {}

core::Status ImportService::validate(const ImportPlan& /*plan*/) const {
    return core::notImplemented("ImportService::validate (Phase 8)");
}

core::Status ImportService::run(const ImportPlan& /*plan*/, std::uint32_t /*jobId*/) {
    (void)storage_;
    (void)mtp_;
    (void)projects_;
    (void)eventQueue_;
    return core::notImplemented("ImportService::run (Phase 8)");
}

void ImportService::requestCancel() noexcept {
    cancelRequested_.store(true, std::memory_order_release);
}

}  // namespace aw::project
