#include "aw/platform/esp32p4/EspConsole.hpp"

#include <unistd.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <string>

#include "esp_log.h"

namespace aw::platform::esp32p4 {
namespace {

int lockedVprintf(const char* format, va_list args) {
    // Format first, then emit with one write, so the line stays whole.
    char line[256];
    const int length = std::vsnprintf(line, sizeof(line), format, args);
    if (length <= 0) return length;
    consoleWrite({line, std::min<std::size_t>(static_cast<std::size_t>(length), sizeof(line) - 1)});
    return length;
}

}  // namespace

std::recursive_mutex& consoleMutex() {
    static std::recursive_mutex mutex;
    return mutex;
}

void consoleWrite(std::string_view text) {
    const std::lock_guard lock{consoleMutex()};
    std::fflush(stdout);
    // stdout's descriptor, not STDOUT_FILENO: ESP-IDF opens /dev/console for
    // stdout at startup and the number it gets is not guaranteed to be 1.
    const int fd = fileno(stdout);
    for (std::size_t sent = 0; sent < text.size();) {
        const ssize_t n = ::write(fd, text.data() + sent, text.size() - sent);
        if (n <= 0) break;
        sent += static_cast<std::size_t>(n);
    }
}

void lockIdfLogging() { esp_log_set_vprintf(&lockedVprintf); }

void EspConsoleLogSink::write(core::LogLevel level, std::string_view module,
                              std::string_view message) {
    const std::string_view name = core::toString(level);
    std::string line = "[" + std::string{name};
    line.append(name.size() < 5 ? 5 - name.size() : 0, ' ');
    line += "] ";
    line += module;
    line.append(module.size() < 10 ? 10 - module.size() : 0, ' ');
    line += ' ';
    line += message;
    line += '\n';
    consoleWrite(line);
}

}  // namespace aw::platform::esp32p4
