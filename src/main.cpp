#include <atomic>
#include <csignal>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include <spdlog/spdlog.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/utsname.h>
#endif

#include "core/certificate.hpp"
#include "core/core.hpp"
#include "core/crash_handler.hpp"
#include "core/logger.hpp"
#include "core/session_log.hpp"

#ifndef GTPROXY_GIT_SHA
#define GTPROXY_GIT_SHA "unknown"
#endif

std::unique_ptr<core::Core> g_core{ nullptr };

namespace {
std::atomic<const char*> g_stop_reason{ nullptr };

void signal_handler(const int signal)
{
    // Only async-signal-safe work here: logging could deadlock if the signal
    // arrives while the main thread holds the logger's lock.
    g_stop_reason.store(signal == SIGINT ? "Ctrl+C" : "termination signal");
    if (g_core) {
        g_core->stop();
    }
}

#ifdef _WIN32
std::atomic<bool> g_shutdown_complete{ false };

// Closing the console window, logging off or shutting down Windows: stop
// cleanly so the session log ends with a proper "Session ended" line.
BOOL WINAPI console_handler(const DWORD type)
{
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
        g_stop_reason.store("Ctrl+C");
        break;
    case CTRL_CLOSE_EVENT:
        g_stop_reason.store("console window closed");
        break;
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        g_stop_reason.store("Windows log off / shutdown");
        break;
    default:
        return FALSE;
    }

    if (g_core) {
        g_core->stop();
    }

    // Windows ends the process when this returns for close/logoff/shutdown;
    // give the main thread a moment to finish and flush the log.
    for (int i = 0; i < 80 && !g_shutdown_complete.load(); ++i) {
        Sleep(50);
    }
    return TRUE;
}

std::string describe_system()
{
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    if (const auto ntdll{ GetModuleHandleW(L"ntdll.dll") }) {
        if (const auto get_version{ reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion")) }) {
            OSVERSIONINFOW info{};
            info.dwOSVersionInfoSize = sizeof(info);
            if (get_version(&info) == 0) {
                return fmt::format("Windows {}.{} build {}", info.dwMajorVersion, info.dwMinorVersion, info.dwBuildNumber);
            }
        }
    }
    return "Windows";
}
#else
std::string describe_system()
{
    utsname info{};
    if (uname(&info) == 0) {
        return fmt::format("{} {} {}", info.sysname, info.release, info.machine);
    }
    return "unknown";
}
#endif

std::string describe_compiler()
{
#if defined(_MSC_VER)
    return fmt::format("MSVC {}", _MSC_VER);
#elif defined(__clang__)
    return fmt::format("Clang {}.{}", __clang_major__, __clang_minor__);
#elif defined(__MINGW32__)
    return fmt::format("MinGW GCC {}.{}", __GNUC__, __GNUC_MINOR__);
#elif defined(__GNUC__)
    return fmt::format("GCC {}.{}", __GNUC__, __GNUC_MINOR__);
#else
    return "unknown";
#endif
}

void log_session_header(const std::shared_ptr<core::session_log::Sink>& session, const int argc, char** argv)
{
    std::string arguments;
    for (int i = 1; i < argc; ++i) {
        arguments += ' ';
        arguments += argv[i];
    }

    std::error_code ec;
    spdlog::info(
        "GTProxy v{}.{}.{} (build {}, {}) on {}",
        GTPROXY_VERSION_MAJOR,
        GTPROXY_VERSION_MINOR,
        GTPROXY_VERSION_PATCH,
        GTPROXY_GIT_SHA,
        describe_compiler(),
        describe_system()
    );
    spdlog::info("Folder: {}{}", std::filesystem::current_path(ec).string(), arguments.empty() ? "" : " | arguments:" + arguments);
    if (session) {
        spdlog::info("Session log: {} (send this file when reporting a problem)", session->path().string());
    }
}

int finish(const int code, const std::string_view how)
{
    g_core.reset();
    spdlog::info("Session ended ({})", how);
    spdlog::shutdown();
#ifdef _WIN32
    g_shutdown_complete.store(true);
#endif
    return code;
}
}

int main(const int argc, char** argv)
{
    std::shared_ptr<core::session_log::Sink> session;

    try {
        core::Logger logger{};

        std::vector<spdlog::sink_ptr> sinks{ core::Logger::create_console_sink() };
        try {
            session = core::session_log::create("logs");
            session->set_level(spdlog::level::debug);
            sinks.push_back(session);
        }
        catch (const std::exception& e) {
            // Keep running without a session log (e.g. read-only folder).
            std::fprintf(stderr, "Could not create the session log: %s\n", e.what());
        }

        logger.set_logger(std::make_shared<spdlog::logger>("GTProxy", sinks.begin(), sinks.end()));
        logger.get_logger()->set_level(spdlog::level::trace);
        // Problems reach the disk at once; everything else at least every second.
        logger.get_logger()->flush_on(spdlog::level::warn);

        spdlog::register_logger(logger.get_logger());
        spdlog::set_default_logger(logger.get_logger());
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%t] [%^%l%$] %v");
        spdlog::flush_every(std::chrono::seconds(1));
    }
    catch (const spdlog::spdlog_ex& ex) {
        spdlog::error("Log initialization failed: {}", ex.what());
        return 1;
    }

    core::crash_handler::install();
    log_session_header(session, argc, argv);

    try {
        // One-off certificate commands (run once before playing).
        if (argc > 1) {
            const std::string_view command{ argv[1] };
            if (command == "--install-cert") {
                const auto paths{ core::certificate::ensure("resources") };
                const bool ok{ core::certificate::install_ca(paths) };
                spdlog::info(ok ? "Local CA trusted for the current user" : "Could not trust the local CA automatically");
                return finish(ok ? 0 : 1, "--install-cert");
            }
            if (command == "--uninstall-cert") {
                const bool ok{ core::certificate::uninstall_ca() };
                return finish(ok ? 0 : 1, "--uninstall-cert");
            }
            spdlog::error("Unknown option '{}'. Options: --install-cert, --uninstall-cert", command);
            return finish(2, "unknown option");
        }

        g_core = std::make_unique<core::Core>();

        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);
#ifdef _WIN32
        SetConsoleCtrlHandler(console_handler, TRUE);
#endif

        g_core->run();

        const char* reason{ g_stop_reason.load() };
        spdlog::info("Stopping GTProxy ({})...", reason ? reason : "stop requested");
    }
    catch (const std::runtime_error& e) {
        spdlog::error("Runtime error: {}", e.what());
        return finish(1, "runtime error");
    }
    catch (const std::exception& ex) {
        spdlog::error("Exception: {}", ex.what());
        return finish(1, "exception");
    }

    const char* reason{ g_stop_reason.load() };
    return finish(0, reason ? reason : "normally");
}
