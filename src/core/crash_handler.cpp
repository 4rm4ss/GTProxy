#include "crash_handler.hpp"

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <string>

#include <spdlog/spdlog.h>

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#elif defined(__GLIBC__) || defined(__APPLE__)
#include <execinfo.h>
#endif

#if defined(__SANITIZE_ADDRESS__)
#define GTPROXY_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define GTPROXY_ASAN 1
#endif
#endif

namespace core::crash_handler {
namespace {
void flush_log()
{
    if (const auto logger{ spdlog::default_logger() }) {
        logger->flush();
    }
}

void terminate_handler()
{
    std::string what{ "unknown" };
    if (const auto exception{ std::current_exception() }) {
        try {
            std::rethrow_exception(exception);
        } catch (const std::exception& e) {
            what = e.what();
        } catch (...) {
            what = "non-standard exception";
        }
    } else {
        what = "std::terminate called without an active exception";
    }

    spdlog::critical("CRASH: unhandled exception: {}", what);
    flush_log();
    std::abort();
}

#ifdef _WIN32
std::string module_and_offset(const void* address)
{
    HMODULE module{ nullptr };
    if (!GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCSTR>(address),
            &module
        ) || !module) {
        return fmt::format("0x{:X}", reinterpret_cast<std::uintptr_t>(address));
    }

    char path[MAX_PATH]{};
    GetModuleFileNameA(module, path, MAX_PATH);
    std::string name{ path };
    if (const auto slash{ name.find_last_of("\\/") }; slash != std::string::npos) {
        name = name.substr(slash + 1);
    }

    std::string result{ fmt::format(
        "{}+0x{:X}",
        name,
        reinterpret_cast<std::uintptr_t>(address) - reinterpret_cast<std::uintptr_t>(module)
    ) };

    // Function and source line when Proxy.pdb sits next to the exe (MSVC builds).
    const auto process{ GetCurrentProcess() };
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256]{};
    auto* symbol{ reinterpret_cast<SYMBOL_INFO*>(buffer) };
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;
    DWORD64 displacement{ 0 };
    if (SymFromAddr(process, reinterpret_cast<DWORD64>(address), &displacement, symbol)) {
        result += fmt::format(" {}+0x{:X}", symbol->Name, displacement);
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD line_displacement{ 0 };
        if (SymGetLineFromAddr64(process, reinterpret_cast<DWORD64>(address), &line_displacement, &line)) {
            std::string file{ line.FileName ? line.FileName : "" };
            if (const auto slash{ file.find_last_of("\\/") }; slash != std::string::npos) {
                file = file.substr(slash + 1);
            }
            result += fmt::format(" ({}:{})", file, line.LineNumber);
        }
    }

    return result;
}

const char* describe(const DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    case 0xE06D7363: return "C++ exception";
    case 0xC0000409: return "stack buffer overrun / fast fail";
    default: return "unknown";
    }
}

LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS* info)
{
    const auto* record{ info->ExceptionRecord };
    spdlog::critical(
        "CRASH: exception 0x{:08X} ({}) at {}",
        static_cast<unsigned>(record->ExceptionCode),
        describe(record->ExceptionCode),
        module_and_offset(record->ExceptionAddress)
    );

    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        spdlog::critical(
            "CRASH: {} of address 0x{:X}",
            record->ExceptionInformation[0] == 0 ? "read" : record->ExceptionInformation[0] == 1 ? "write" : "execute",
            static_cast<std::uintptr_t>(record->ExceptionInformation[1])
        );
    }

    void* frames[48]{};
    const auto count{ RtlCaptureStackBackTrace(0, 48, frames, nullptr) };
    std::string stack;
    for (USHORT i = 0; i < count; ++i) {
        stack += fmt::format("\n  #{} {}", i, module_and_offset(frames[i]));
    }
    spdlog::critical("CRASH: call stack:{}", stack);
    flush_log();

    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

void fatal_signal_handler(const int signal)
{
    // Best effort: the process is going down anyway.
    spdlog::critical("CRASH: fatal signal {}", signal);
#if !defined(_WIN32) && (defined(__GLIBC__) || defined(__APPLE__))
    void* frames[48]{};
    const int count{ backtrace(frames, 48) };
    if (char** symbols{ backtrace_symbols(frames, count) }) {
        std::string stack;
        for (int i = 0; i < count; ++i) {
            stack += fmt::format("\n  #{} {}", i, symbols[i]);
        }
        spdlog::critical("CRASH: call stack:{}", stack);
        std::free(symbols);
    }
#endif
    flush_log();
    std::signal(signal, SIG_DFL);
    std::raise(signal);
}
}

void install()
{
    std::set_terminate(terminate_handler);

#ifdef _WIN32
    // Symbols load lazily, only if a crash report is actually written.
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    SetUnhandledExceptionFilter(unhandled_exception_filter);
    std::signal(SIGABRT, fatal_signal_handler);
#elif !defined(GTPROXY_ASAN)
    // (AddressSanitizer builds keep ASan's own, more detailed reports.)
    for (const int signal : { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT }) {
        std::signal(signal, fatal_signal_handler);
    }
#endif
}
}
