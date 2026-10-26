#pragma once

#include "aw/devices/ApogeeDuet.hpp"
#include "aw/events/EventQueue.hpp"
#include "aw/ui/DirtyFlag.hpp"
#include "aw/ui/Screen.hpp"
#include "aw/ui/TaskRunner.hpp"

#include <array>
#include <atomic>

namespace aw::ui {

/// The input types of an Apogee Duet plugged into Bridge's USB port.
///
/// Turn to pick an input, push to edit it, turn to pick a type, push to send
/// it; Back cancels the edit. Only the chosen type is sent, never the ones
/// passed on the way, so the Duet does not switch through its microphone
/// stage while the knob turns. Every read and write runs on the I/O worker,
/// and the screen shows what the Duet reports back.
class DuetScreen final : public Screen {
public:
    DuetScreen(devices::ApogeeDuet& duet, TaskRunner& tasks,
               events::EventQueue& workerQueue) noexcept;

    [[nodiscard]] ScreenId id() const noexcept override { return ScreenId::Duet; }
    [[nodiscard]] std::string_view title() const noexcept override { return "Duet"; }

    void onEnter() override;
    [[nodiscard]] NavigationRequest handleEvent(const events::Event& event) override;
    [[nodiscard]] bool isDirty() const noexcept override { return dirty_.isSet(); }
    void render(display::Painter& painter) override;

private:
    enum class Stage : std::uint8_t { Busy, Ready, Editing };

    /// Reads both inputs' types, after setting `input` to `type` if given.
    void submit(int input = -1, devices::DuetInputType type = devices::DuetInputType::Line4dBu);

    devices::ApogeeDuet& duet_;
    TaskRunner& tasks_;
    events::EventQueue& workerQueue_;
    DirtyFlag dirty_;
    Stage stage_{Stage::Busy};
    int selection_{0};
    int pending_{0};  ///< The type being chosen while editing.
    std::uint32_t jobId_{0};
    core::ErrorCode lastError_{core::ErrorCode::Unknown};

    /// Written by the I/O worker before it posts the job's event; -1 unknown.
    std::array<std::atomic<int>, devices::ApogeeDuet::kInputCount> types_{};
};

}  // namespace aw::ui
