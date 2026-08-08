#include "aw/ui/Screen.hpp"

namespace aw::ui {

std::string_view toString(ScreenId id) noexcept {
    switch (id) {
        case ScreenId::Home: return "Home";
        case ScreenId::ProjectBrowser: return "ProjectBrowser";
        case ScreenId::TrackView: return "TrackView";
        case ScreenId::Playback: return "Playback";
        case ScreenId::Import: return "Import";
        case ScreenId::Export: return "Export";
        case ScreenId::CopyChannel: return "CopyChannel";
        case ScreenId::Settings: return "Settings";
    }
    return "?";
}

}  // namespace aw::ui
