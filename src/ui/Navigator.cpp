#include "aw/ui/Navigator.hpp"

#include <cassert>

namespace aw::ui {

void Navigator::registerScreen(std::unique_ptr<Screen> screen) {
    if (!screen) {
        return;
    }
    const auto key = static_cast<std::uint8_t>(screen->id());
    screens_[key] = std::move(screen);
}

void Navigator::reset(ScreenId id) {
    if (Screen* previous = current()) {
        previous->onExit();
    }
    stack_.clear();
    stack_.push_back(id);
    if (Screen* next = current()) {
        next->onEnter();
    }
}

Screen* Navigator::current() noexcept {
    if (stack_.empty()) {
        return nullptr;
    }
    const auto it = screens_.find(static_cast<std::uint8_t>(stack_.back()));
    return it == screens_.end() ? nullptr : it->second.get();
}

const Screen* Navigator::current() const noexcept {
    if (stack_.empty()) {
        return nullptr;
    }
    const auto it = screens_.find(static_cast<std::uint8_t>(stack_.back()));
    return it == screens_.end() ? nullptr : it->second.get();
}

void Navigator::push(ScreenId id) {
    // Exceeding the depth limit means the UI has grown a menu tree. Refuse it
    // here rather than let it creep in one screen at a time.
    assert(stack_.size() < kMaxDepth && "navigation deeper than the UI design allows");
    if (stack_.size() >= kMaxDepth) {
        replace(id);
        return;
    }
    if (Screen* previous = current()) {
        previous->onExit();
    }
    stack_.push_back(id);
    if (Screen* next = current()) {
        next->onEnter();
    }
}

void Navigator::pop() {
    if (stack_.size() <= 1) {
        return;  // Home never pops off; Back at Home does nothing.
    }
    if (Screen* previous = current()) {
        previous->onExit();
    }
    stack_.pop_back();
    if (Screen* next = current()) {
        next->onEnter();
    }
}

void Navigator::replace(ScreenId id) {
    if (stack_.empty()) {
        reset(id);
        return;
    }
    if (Screen* previous = current()) {
        previous->onExit();
    }
    stack_.back() = id;
    if (Screen* next = current()) {
        next->onEnter();
    }
}

bool Navigator::apply(const NavigationRequest& request) {
    switch (request.intent) {
        case NavigationIntent::None:
        case NavigationIntent::Ignored:
            return false;
        case NavigationIntent::Back:
            pop();
            return true;
        case NavigationIntent::Push:
            push(request.requestedScreen);
            return true;
        case NavigationIntent::Replace:
            replace(request.requestedScreen);
            return true;
    }
    return false;
}

}  // namespace aw::ui
