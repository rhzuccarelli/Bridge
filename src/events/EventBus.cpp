#include "aw/events/EventBus.hpp"

#include <algorithm>

namespace aw::events {

void EventBus::addSource(EventQueue& queue) {
    if (std::find(sources_.begin(), sources_.end(), &queue) == sources_.end()) {
        sources_.push_back(&queue);
    }
}

SubscriptionId EventBus::subscribe(Handler handler) {
    const SubscriptionId id = nextId_++;
    subscribers_.push_back(Subscriber{id, std::move(handler)});
    return id;
}

void EventBus::unsubscribe(SubscriptionId id) {
    subscribers_.erase(std::remove_if(subscribers_.begin(), subscribers_.end(),
                                      [id](const Subscriber& s) { return s.id == id; }),
                       subscribers_.end());
}

std::size_t EventBus::dispatchPending(std::size_t budget) {
    std::size_t delivered = 0;

    // Round-robin across sources so a chatty producer cannot starve the others.
    bool anySourceHadWork = true;
    while (delivered < budget && anySourceHadWork) {
        anySourceHadWork = false;
        for (EventQueue* source : sources_) {
            if (delivered >= budget) {
                break;
            }
            if (auto event = source->poll()) {
                anySourceHadWork = true;
                ++delivered;
                publishDirect(*event);
            }
        }
    }
    return delivered;
}

void EventBus::publishDirect(const Event& event) {
    // Iterate by index: a handler may unsubscribe itself, which can reallocate.
    for (std::size_t i = 0; i < subscribers_.size(); ++i) {
        if (subscribers_[i].handler) {
            subscribers_[i].handler(event);
        }
    }
}

}  // namespace aw::events
