#include "aw/core/Controls.hpp"

namespace aw::core {

std::string_view toString(ButtonId id) noexcept {
    switch (id) {
        case ButtonId::Play: return "Play";
        case ButtonId::Stop: return "Stop";
        case ButtonId::Record: return "Record";
        case ButtonId::Import: return "Import";
        case ButtonId::Export: return "Export";
        case ButtonId::Undo: return "Undo";
        case ButtonId::Save: return "Save";
        case ButtonId::Back: return "Back";
        case ButtonId::Track1: return "Track1";
        case ButtonId::Track2: return "Track2";
        case ButtonId::Track3: return "Track3";
        case ButtonId::Track4: return "Track4";
        case ButtonId::EncoderPush: return "EncoderPush";
        case ButtonId::Count: return "Count";
    }
    return "?";
}

std::string_view toString(ButtonAction action) noexcept {
    switch (action) {
        case ButtonAction::Pressed: return "Pressed";
        case ButtonAction::Released: return "Released";
        case ButtonAction::LongPressed: return "LongPressed";
        case ButtonAction::Repeat: return "Repeat";
    }
    return "?";
}

}  // namespace aw::core
