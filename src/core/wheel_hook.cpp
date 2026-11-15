#include "wheel_hook.hpp"

#include <utility>

#include <spdlog/spdlog.h>

#ifdef _WIN32
#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <future>
#include <map>
#include <string>
#endif

namespace core {
#ifdef _WIN32
namespace {
// Shared with the hook procedure (one hook per proxy).
std::atomic<int> g_modifier{ -1 };     // -1 off, 0 always, else the VK_ that must be held
std::mutex g_mutex;
std::vector<WheelHook::Turn> g_turns;
int g_remainder{ 0 };
std::map<HWND, bool> g_game_windows;   // only used on the hook thread

bool is_game_window(const HWND window)
{
    if (const auto it{ g_game_windows.find(window) }; it != g_game_windows.end()) {
        return it->second;
    }

    bool game{ false };
    DWORD process_id{ 0 };
    GetWindowThreadProcessId(window, &process_id);
    if (const HANDLE process{ OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id) }) {
        wchar_t path[MAX_PATH]{};
        DWORD size{ MAX_PATH };
        if (QueryFullProcessImageNameW(process, 0, path, &size)) {
            std::wstring name{ path, size };
            name = name.substr(name.find_last_of(L"\\\\/") + 1);
            std::ranges::transform(name, name.begin(), [](const wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            game = name == L"growtopia.exe";
        }
        CloseHandle(process);
    }
    if (g_game_windows.size() > 64) {
        g_game_windows.clear();
    }
    g_game_windows[window] = game;
    return game;
}

LRESULT CALLBACK on_mouse(const int code, const WPARAM message, const LPARAM data)
{
    const int modifier{ g_modifier.load() };
    if (code == HC_ACTION && message == WM_MOUSEWHEEL && modifier >= 0) {
        const auto* info{ reinterpret_cast<const MSLLHOOKSTRUCT*>(data) };
        const bool held{ modifier == 0 || (GetAsyncKeyState(modifier) & 0x8000) != 0 };
        const HWND window{ WindowFromPoint(info->pt) };
        if (held && window && is_game_window(GetAncestor(window, GA_ROOT))) {
            const auto delta{ static_cast<short>(HIWORD(info->mouseData)) };
            {
                const std::lock_guard lock{ g_mutex };
                g_remainder += delta;
                const int notches{ g_remainder / WHEEL_DELTA };
                if (notches != 0) {
                    g_remainder -= notches * WHEEL_DELTA;
                    g_turns.push_back({ notches });
                }
            }
            return 1;   // the game does not see it
        }
    }
    return CallNextHookEx(nullptr, code, message, data);
}

int modifier_key(const std::string_view mode)
{
    if (mode == "always") return 0;
    if (mode == "ctrl") return VK_CONTROL;
    if (mode == "shift") return VK_SHIFT;
    if (mode == "alt") return VK_MENU;
    return -1;
}
}

bool WheelHook::available()
{
    return true;
}

WheelHook::~WheelHook()
{
    g_modifier = -1;
    stop();
}

bool WheelHook::set_mode(const std::string_view mode)
{
    const int modifier{ modifier_key(mode) };
    if (mode_ == mode) {
        return true;
    }
    mode_ = std::string{ mode };
    g_modifier = modifier;
    if (modifier >= 0) {
        start();
        spdlog::info("Mouse wheel over the Growtopia window: taken by the proxy ({})", mode);
    }
    else {
        stop();
        spdlog::info("Mouse wheel over the Growtopia window: back to the game");
    }
    return true;
}

void WheelHook::start()
{
    if (thread_.joinable()) {
        return;
    }
    std::promise<unsigned long> started;
    auto id{ started.get_future() };
    thread_ = std::thread{ [&started] {
        const HHOOK hook{ SetWindowsHookExW(WH_MOUSE_LL, on_mouse, GetModuleHandleW(nullptr), 0) };
        if (!hook) {
            spdlog::error("Mouse wheel: the mouse hook could not be set (error {})", GetLastError());
        }
        MSG message{};
        PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // the thread's queue exists now
        started.set_value(GetCurrentThreadId());
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (hook) {
            UnhookWindowsHookEx(hook);
        }
    } };
    thread_id_ = id.get();
}

void WheelHook::stop()
{
    if (!thread_.joinable()) {
        return;
    }
    PostThreadMessageW(thread_id_, WM_QUIT, 0, 0);
    thread_.join();
    thread_id_ = 0;
}

std::vector<WheelHook::Turn> WheelHook::take()
{
    const std::lock_guard lock{ g_mutex };
    return std::exchange(g_turns, {});
}
#else
bool WheelHook::available()
{
    return false;
}

WheelHook::~WheelHook() = default;

bool WheelHook::set_mode(const std::string_view mode)
{
    mode_ = std::string{ mode };
    return false;
}

void WheelHook::start() { }
void WheelHook::stop() { }

std::vector<WheelHook::Turn> WheelHook::take()
{
    return {};
}
#endif
}
