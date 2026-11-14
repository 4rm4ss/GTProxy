#include "browser.hpp"

#include <cstdlib>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace core::browser {
bool open_window(const std::string& url)
{
#ifdef _WIN32
    // The address is the proxy's own http://127.0.0.1:<port>/ (ASCII).
    const std::wstring wide_url(url.begin(), url.end());
    const std::wstring app_args{ L"--app=" + wide_url + L" --window-size=1100,760" };

    const auto edge{ reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", L"msedge.exe", app_args.c_str(), nullptr, SW_SHOWNORMAL)
    ) };
    if (edge > 32) {
        return true;
    }

    const auto browser{ reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", wide_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL)
    ) };
    if (browser > 32) {
        return true;
    }

    spdlog::warn("Could not open {} (ShellExecute error {})", url, static_cast<long long>(browser));
    return false;
#else
#ifdef __APPLE__
    const std::string command{ fmt::format("open '{}'", url) };
#else
    const std::string command{ fmt::format("xdg-open '{}' >/dev/null 2>&1 &", url) };
#endif
    if (std::system(command.c_str()) != 0) {
        spdlog::warn("Could not open {} - open it in a browser yourself", url);
        return false;
    }
    return true;
#endif
}
}
