#pragma once

#include <memory>
#include <string>
#include <string_view>

namespace aw::core {

enum class LogLevel : std::uint8_t { Trace, Debug, Info, Warning, Error, Off };

[[nodiscard]] std::string_view toString(LogLevel level) noexcept;

/// Where log lines go.  Implemented by stderr on the Pi, by UART on the ESP32,
/// and by a capture buffer in tests.
class LogSink {
public:
    virtual ~LogSink() = default;
    virtual void write(LogLevel level, std::string_view module, std::string_view message) = 0;
};

/// Process wide logging facade.
///
/// This is the project's *one* deliberate global.  The alternative - threading
/// an `ILogger&` through every constructor - was rejected: it triples the
/// constructor surface of small classes and makes logging so awkward that it
/// stops happening.  The global holds only a sink pointer, is installed once at
/// startup by the composition root, and is never read for control flow, so it
/// does not create the hidden coupling that makes singletons a problem.
///
/// Not safe to call from the realtime audio thread (formatting allocates).
namespace log {

void installSink(std::shared_ptr<LogSink> sink);
void setLevel(LogLevel level);
[[nodiscard]] LogLevel level() noexcept;

void write(LogLevel level, std::string_view module, std::string_view message);

inline void trace(std::string_view module, std::string_view message) {
    write(LogLevel::Trace, module, message);
}
inline void debug(std::string_view module, std::string_view message) {
    write(LogLevel::Debug, module, message);
}
inline void info(std::string_view module, std::string_view message) {
    write(LogLevel::Info, module, message);
}
inline void warning(std::string_view module, std::string_view message) {
    write(LogLevel::Warning, module, message);
}
inline void error(std::string_view module, std::string_view message) {
    write(LogLevel::Error, module, message);
}

}  // namespace log

/// Writes to stderr.  Used by the simulator and the Pi build.
class ConsoleLogSink final : public LogSink {
public:
    void write(LogLevel level, std::string_view module, std::string_view message) override;
};

}  // namespace aw::core
