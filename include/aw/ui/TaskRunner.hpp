#pragma once

#include <functional>

#include "aw/core/Result.hpp"

namespace aw::ui {

/// Single background I/O queue. Tasks execute in submission order and may block;
/// they communicate results back to the main thread only through EventQueue.
class TaskRunner {
public:
    virtual ~TaskRunner() = default;
    [[nodiscard]] virtual core::Status submit(std::function<void()> task) = 0;
};

}  // namespace aw::ui
