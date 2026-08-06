#include "aw/platform/desktop/VirtualPanel.hpp"

#include <algorithm>

namespace aw::platform::desktop {
namespace {

/// Two rows of six, in the order they would sit on a front panel: transport and
/// edit actions on top, track selection and transfer below.
constexpr std::array<std::pair<core::ButtonId, std::string_view>, 12> kKeyOrder{{
    {core::ButtonId::Play, "PLAY"},
    {core::ButtonId::Stop, "STOP"},
    {core::ButtonId::Record, "REC"},
    {core::ButtonId::Undo, "UNDO"},
    {core::ButtonId::Save, "SAVE"},
    {core::ButtonId::Back, "BACK"},
    {core::ButtonId::Track1, "TR1"},
    {core::ButtonId::Track2, "TR2"},
    {core::ButtonId::Track3, "TR3"},
    {core::ButtonId::Track4, "TR4"},
    {core::ButtonId::Import, "IMP"},
    {core::ButtonId::Export, "EXP"},
}};

constexpr int kColumns = 6;
constexpr int kRows = 2;
constexpr std::int16_t kMinimumRowHeight = 24;

}  // namespace

VirtualPanelLayout::VirtualPanelLayout(std::uint16_t panelWidth,
                                       std::uint16_t panelHeight,
                                       std::uint16_t canvasWidth,
                                       std::uint16_t canvasHeight) {
    const auto panelW = static_cast<std::int16_t>(panelWidth);
    const auto panelH = static_cast<std::int16_t>(panelHeight);
    const auto canvasW = static_cast<std::int16_t>(std::min(canvasWidth, panelWidth));
    const auto canvasH = static_cast<std::int16_t>(std::min(canvasHeight, panelHeight));

    // Canvas centred horizontally, flush to the top: the leftover vertical space
    // then forms one contiguous strip rather than two useless bands.
    canvasArea_ = hal::Rect{static_cast<std::int16_t>((panelW - canvasW) / 2), 0, canvasW,
                            canvasH};

    const auto stripHeight = static_cast<std::int16_t>(panelH - canvasH);
    if (stripHeight < kMinimumRowHeight * kRows) {
        keyStripArea_ = hal::Rect{};  // No room; keyboard or real buttons only.
        return;
    }

    keyStripArea_ = hal::Rect{0, canvasH, panelW, stripHeight};

    const auto keyWidth = static_cast<std::int16_t>(panelW / kColumns);
    const auto keyHeight = static_cast<std::int16_t>(stripHeight / kRows);

    keys_.reserve(kKeyOrder.size());
    for (std::size_t i = 0; i < kKeyOrder.size(); ++i) {
        const auto column = static_cast<std::int16_t>(i % kColumns);
        const auto row = static_cast<std::int16_t>(i / kColumns);
        keys_.push_back(VirtualKey{
            kKeyOrder[i].first,
            hal::Rect{static_cast<std::int16_t>(column * keyWidth),
                      static_cast<std::int16_t>(canvasH + row * keyHeight), keyWidth, keyHeight},
            kKeyOrder[i].second});
    }
}

std::optional<core::ButtonId> VirtualPanelLayout::hitTest(std::int16_t x,
                                                          std::int16_t y) const noexcept {
    for (const VirtualKey& key : keys_) {
        if (x >= key.area.x && x < key.area.x + key.area.width && y >= key.area.y &&
            y < key.area.y + key.area.height) {
            return key.id;
        }
    }
    return std::nullopt;
}

}  // namespace aw::platform::desktop
