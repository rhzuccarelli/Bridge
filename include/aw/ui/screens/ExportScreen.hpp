#pragma once

#include "aw/hal/DeviceModeInterface.hpp"
#include "aw/mtp/MtpManager.hpp"
#include "aw/mtp/MtpBrowser.hpp"
#include "aw/project/ExportService.hpp"
#include "aw/project/ProjectManager.hpp"
#include "aw/ui/DirtyFlag.hpp"
#include "aw/ui/Screen.hpp"
#include "aw/ui/TaskRunner.hpp"

#include <atomic>

namespace aw::ui {

/// Choose tracks to render and write back to the TP-7.
///
/// Two states: selection, then - if any target name already exists on the
/// device - an explicit overwrite confirmation listing exactly which files
/// would be replaced.  There is no "always overwrite" setting, on purpose:
/// the device holds recordings that may not exist anywhere else.
class ExportScreen final : public Screen {
public:
    ExportScreen(mtp::MtpManager& manager,
                 mtp::MtpBrowser& browser,
                 project::ProjectManager& projects,
                 project::ExportService& exporter,
                 TaskRunner& tasks,
                 events::EventQueue& workerQueue,
                 hal::DeviceModeInterface* deviceMode = nullptr) noexcept;

    [[nodiscard]] ScreenId id() const noexcept override { return ScreenId::Export; }
    [[nodiscard]] std::string_view title() const noexcept override { return "Export"; }

    void onEnter() override;
    void onExit() override;
    [[nodiscard]] NavigationRequest handleEvent(const events::Event& event) override;
    void update(std::uint32_t elapsedMillis) override;
    [[nodiscard]] bool isDirty() const noexcept override { return dirty_.isSet(); }
    void render(display::Painter& painter) override;

private:
    enum class Stage : std::uint8_t {
        SwitchingMode,
        WaitingForMtp,
        Connecting,
        Selecting,
        Checking,
        ConfirmingOverwrite,
        Transferring,
    };

    void requestMtpMode();
    void startScan();

    mtp::MtpManager& manager_;
    mtp::MtpBrowser& browser_;
    project::ProjectManager& projects_;
    project::ExportService& exporter_;
    TaskRunner& tasks_;
    events::EventQueue& workerQueue_;
    hal::DeviceModeInterface* deviceMode_;
    DirtyFlag dirty_;
    Stage stage_{Stage::Selecting};
    int selection_{0};
    std::uint8_t progressPercent_{0};
    std::uint32_t jobId_{0};
    std::uint32_t waitingMillis_{0};
    std::atomic<hal::MtpStorageId> storageId_{0};
    std::atomic<hal::MtpObjectHandle> folderId_{hal::kMtpRootHandle};
    core::ErrorCode lastError_{core::ErrorCode::Unknown};
};

}  // namespace aw::ui
