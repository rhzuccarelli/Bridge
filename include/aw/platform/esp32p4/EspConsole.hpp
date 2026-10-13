#pragma once

#include <mutex>
#include <string_view>

#include "aw/core/Logger.hpp"

namespace aw::platform::esp32p4 {

/// The serial console has three writers on different tasks: Bridge's logger,
/// ESP-IDF's own ESP_LOGx output and the display preview. Nothing in the UART
/// path keeps their bytes apart, so without a common lock they interleave
/// character by character. Every console write takes this mutex for one whole
/// line.
std::recursive_mutex& consoleMutex();

/// Writes `text` with a single write() under `consoleMutex()`.
void consoleWrite(std::string_view text);

/// Routes ESP-IDF's log output through `consoleMutex()`. Idempotent.
void lockIdfLogging();

/// `core::ConsoleLogSink` with the same line format, under `consoleMutex()`.
class EspConsoleLogSink final : public core::LogSink {
public:
    void write(core::LogLevel level, std::string_view module, std::string_view message) override;
};

}  // namespace aw::platform::esp32p4
