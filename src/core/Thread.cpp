#include "aw/core/Thread.hpp"

#include <atomic>
#include <utility>

namespace aw::core::threads {
namespace {

std::atomic<ThreadPlacer> placer{nullptr};

}  // namespace

void installPlacer(ThreadPlacer next) noexcept { placer.store(next); }

std::thread start(ThreadRole role, std::function<void()> body) {
    const ThreadPlacer place = placer.load();
    if (place == nullptr || role == ThreadRole::General) return std::thread{std::move(body)};
    place(role);
    std::thread thread{std::move(body)};
    place(ThreadRole::General);  // Later threads from this one are placed as usual.
    return thread;
}

}  // namespace aw::core::threads
