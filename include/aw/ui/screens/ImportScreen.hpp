#pragma once

#include <atomic>

#include "aw/mtp/MtpBrowser.hpp"
#include "aw/mtp/MtpManager.hpp"
#include "aw/project/ImportService.hpp"
#include "aw/ui/DirtyFlag.hpp"
#include "aw/ui/Screen.hpp"
#include "aw/ui/TaskRunner.hpp"

namespace aw::ui {

/// Browse the connected TP-7 and choose recordings to copy in.
///
/// Flow: encoder scrolls; first push selects a file; pushing the selected file
/// again confirms the import. Long encoder press goes back.
///
/// The screen holds no MTP state: it renders `browser.entries()`.  When no
/// device is connected it says so and offers nothing else, which is a better
/// answer than a list of stale entries from the last connection.
class ImportScreen final : public Screen {
public:
    ImportScreen(mtp::MtpManager& manager,
                 mtp::MtpBrowser& browser,
                 project::ImportService& importer,
                 TaskRunner& tasks,
                 events::EventQueue& workerQueue) noexcept;

    [[nodiscard]] ScreenId id() const noexcept override { return ScreenId::Import; }
    [[nodiscard]] std::string_view title() const noexcept override { return "Import"; }

    void onEnter() override;
    [[nodiscard]] NavigationRequest handleEvent(const events::Event& event) override;
    void update(std::uint32_t elapsedMillis) override;
    [[nodiscard]] bool isDirty() const noexcept override { return dirty_.isSet(); }
    void render(display::Painter& painter) override;

private:
    mtp::MtpManager& manager_;
    mtp::MtpBrowser& browser_;
    project::ImportService& importer_;
    TaskRunner& tasks_;
    events::EventQueue& workerQueue_;
    DirtyFlag dirty_;
    int selection_{0};
    int scrollOffset_{0};
    bool transferInProgress_{false};
    std::uint8_t progressPercent_{0};
    std::uint32_t jobId_{0};
    bool scanning_{false};
    bool importCompleted_{false};
    std::atomic<bool> workerCompletionPending_{false};
    std::atomic<bool> workerSucceeded_{false};
    std::atomic<core::ErrorCode> workerError_{core::ErrorCode::Unknown};
    core::ErrorCode lastError_{core::ErrorCode::Unknown};
};

}  // namespace aw::ui
