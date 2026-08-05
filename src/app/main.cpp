///
/// Entry point and composition root.
///
/// The only file that mentions a concrete platform.  Its job is to build a
/// `hal::Platform`, hand it to `Application`, and translate signals into a stop
/// request.  Everything else the device does happens behind interfaces.
///
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <memory>
#include <string>

#include "aw/app/Application.hpp"
#include "aw/core/Logger.hpp"

#if defined(AW_PLATFORM_RPI)
#include "aw/platform/rpi/RpiPlatform.hpp"
#else
#include "aw/platform/simulator/SimulatorPlatform.hpp"
#endif

namespace {

/// A signal handler may only touch a lock-free atomic, so the pointer to the
/// running application is kept here and used for nothing else.
std::atomic<aw::app::Application*> g_application{nullptr};

extern "C" void handleSignal(int /*signal*/) {
    if (aw::app::Application* application = g_application.load(std::memory_order_acquire)) {
        application->requestStop();
    }
}

aw::core::Path projectsRootFromEnvironment(const char* fallback) {
    if (const char* value = std::getenv("AW_PROJECTS_ROOT")) {
        return aw::core::Path{value};
    }
    return aw::core::Path{fallback};
}

}  // namespace

int main() {
    using namespace aw;

    core::log::installSink(std::make_shared<core::ConsoleLogSink>());
    core::log::setLevel(core::LogLevel::Info);

#if defined(AW_PLATFORM_RPI)
    platform::rpi::RpiConfig config;
    config.projectsRoot = projectsRootFromEnvironment("/var/lib/audio-workstation/projects");
    auto platform = platform::rpi::createPlatform(config);
#else
    auto platform = platform::simulator::createPlatform(
        projectsRootFromEnvironment("./projects"));
#endif

    if (!platform) {
        core::log::error("main", "platform init failed: " + platform.error().describe());
        return EXIT_FAILURE;
    }

    app::Application application{std::move(platform).value()};
    g_application.store(&application, std::memory_order_release);
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    if (auto status = application.initialise(); !status) {
        core::log::error("main", "initialise failed: " + status.error().describe());
        return EXIT_FAILURE;
    }

    const auto result = application.run();

    g_application.store(nullptr, std::memory_order_release);
    application.shutdown();

    if (!result) {
        core::log::error("main", "run failed: " + result.error().describe());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
