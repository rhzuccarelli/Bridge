///
/// ESP32-P4 entry point and composition root.
///
/// The device counterpart of src/app/main.cpp: build a `hal::Platform`, hand it
/// to `Application`, run. There are no signals to translate, so the main loop
/// runs until power-off.
///
#include <cstdlib>
#include <memory>

#include "aw/app/Application.hpp"
#include "aw/core/Logger.hpp"
#include "aw/platform/esp32p4/Esp32P4Platform.hpp"
#include "aw/platform/esp32p4/EspConsole.hpp"
#include "esp_heap_caps.h"

extern "C" void app_main(void) {
    using namespace aw;

    // One lock for every console writer, so log lines and the display
    // preview never interleave mid-line.
    platform::esp32p4::lockIdfLogging();
    core::log::installSink(std::make_shared<platform::esp32p4::EspConsoleLogSink>());
    core::log::setLevel(core::LogLevel::Info);

    auto platform = platform::esp32p4::createPlatform(platform::esp32p4::Esp32P4Config{});
    if (!platform) {
        core::log::error("main", "platform init failed: " + platform.error().describe());
        return;
    }

    // On the heap: the event queues make Application too large for a task stack.
    auto application = std::make_unique<app::Application>(std::move(platform).value());
    if (auto status = application->initialise(); !status) {
        core::log::error("main", "initialise failed: " + status.error().describe());
        return;
    }
    core::log::info("main", "free heap: internal " +
                                std::to_string(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024) +
                                " KiB, PSRAM " +
                                std::to_string(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024) +
                                " KiB");

    if (auto status = application->run(); !status) {
        core::log::error("main", "run failed: " + status.error().describe());
    }
    application->shutdown();
}
