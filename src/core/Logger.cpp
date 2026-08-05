#include "aw/core/Logger.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>

namespace aw::core {
namespace {

struct LogState {
    std::mutex mutex;
    std::shared_ptr<LogSink> sink;
    std::atomic<LogLevel> level{LogLevel::Info};
};

LogState& state() {
    static LogState instance;  // Function-local: no static init order problem.
    return instance;
}

}  // namespace

std::string_view toString(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warning: return "WARN";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Off: return "OFF";
    }
    return "?";
}

namespace log {

void installSink(std::shared_ptr<LogSink> sink) {
    auto& s = state();
    const std::lock_guard<std::mutex> guard{s.mutex};
    s.sink = std::move(sink);
}

void setLevel(LogLevel level) { state().level.store(level, std::memory_order_relaxed); }

LogLevel level() noexcept { return state().level.load(std::memory_order_relaxed); }

void write(LogLevel messageLevel, std::string_view module, std::string_view message) {
    auto& s = state();
    if (messageLevel < s.level.load(std::memory_order_relaxed)) {
        return;
    }
    const std::lock_guard<std::mutex> guard{s.mutex};
    if (s.sink) {
        s.sink->write(messageLevel, module, message);
    }
}

}  // namespace log

void ConsoleLogSink::write(LogLevel level, std::string_view module, std::string_view message) {
    std::fprintf(stderr, "[%-5.*s] %-10.*s %.*s\n",
                 static_cast<int>(toString(level).size()), toString(level).data(),
                 static_cast<int>(module.size()), module.data(),
                 static_cast<int>(message.size()), message.data());
}

}  // namespace aw::core
