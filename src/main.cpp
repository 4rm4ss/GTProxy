#include <csignal>
#include <spdlog/spdlog.h>

#include "core/core.hpp"
#include "core/logger.hpp"

std::unique_ptr<core::Core> g_core{ nullptr };

void signal_handler(int)
{
    // Only async-signal-safe work here: logging could deadlock if the signal
    // arrives while the main thread holds the logger's lock.
    if (g_core) {
        g_core->stop();
    }
}

int main()
{
try {
        core::Logger logger{};

        std::vector<spdlog::sink_ptr> sinks{
            core::Logger::create_console_sink(),
            core::Logger::create_file_sink()
        };

        logger.set_logger(std::make_shared<spdlog::logger>("GTProxy", sinks.begin(), sinks.end()));
        logger.get_logger()->set_level(spdlog::level::trace);

        spdlog::register_logger(logger.get_logger());
        spdlog::set_default_logger(logger.get_logger());
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%t] [%^%l%$] %v");
    }
    catch (const spdlog::spdlog_ex& ex) {
        spdlog::error("Log initialization failed: {}", ex.what());
        return 1;
    }

    try {
        spdlog::info(
            "Starting GTProxy (v{}.{}.{}{})",
            GTPROXY_VERSION_MAJOR,
            GTPROXY_VERSION_MINOR,
            GTPROXY_VERSION_PATCH,
#ifdef GTPROXY_BUILD_VERSION
            fmt::format("-{}", GTPROXY_BUILD_VERSION)
#else
            ""
#endif
        );

        g_core = std::make_unique<core::Core>();

        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);

        g_core->run();
        spdlog::info("Stopping GTProxy...");
    }
    catch (const std::runtime_error& e) {
        spdlog::error("Runtime error: {}", e.what());
        g_core.reset();
        spdlog::shutdown();
        return 1;
    }
    catch (const std::exception& ex) {
        spdlog::error("Exception: {}", ex.what());
        g_core.reset();
        spdlog::shutdown();
        return 1;
    }

    // Destroy the core while the logger and other statics are still alive
    // (a global would otherwise be destroyed after main returns).
    g_core.reset();

    spdlog::info("GTProxy stopped");
    spdlog::shutdown();
    return 0;
}
