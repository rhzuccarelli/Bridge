#pragma once

#include "aw/mtp/MtpBrowser.hpp"
#include "aw/mtp/MtpManager.hpp"
#include "aw/project/ImportService.hpp"
#include "aw/ui/DirtyFlag.hpp"
#include "aw/ui/Screen.hpp"

namespace aw::ui {

/// Browse the connected TP-7 and choose recordings to copy in.
///
/// Flow: encoder scrolls, push toggles selection on a file or enters a folder,
/// Import confirms.  Selection is explicit and multi - the user picks takes,
/// then commits once, rather than triggering a transfer per press.
///
/// The screen holds no MTP state: it renders `browser.entries()`.  When no
/// device is connected it says so and offers nothing else, which is a better
/// answer than a list of stale entries from the last connection.
class ImportScreen final : public Screen {
public:
    ImportScreen(mtp::MtpManager& manager,
                 mtp::MtpBrowser& browser,
                 project::ImportService& importer) noexcept;

    [[nodiscard]] ScreenId id() const noexcept override { return ScreenId::Import; }
    [[nodiscard]] std::string_view title() const noexcept override { return "Import"; }

    void onEnter() override;
    [[nodiscard]] NavigationRequest handleEvent(const events::Event& event) override;
    [[nodiscard]] bool isDirty() const noexcept override { return dirty_.isSet(); }
    void render(display::Painter& painter) override;

private:
    mtp::MtpManager& manager_;
    mtp::MtpBrowser& browser_;
    project::ImportService& importer_;
    DirtyFlag dirty_;
    int selection_{0};
    int scrollOffset_{0};
    bool transferInProgress_{false};
    std::uint8_t progressPercent_{0};
};

}  // namespace aw::ui
