#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace core {
// The mouse wheel over the game's window (Windows: a low-level mouse hook
// on its own thread). While capturing, turns of the wheel over a window of
// Growtopia.exe never reach the game; the main loop takes them with take().
// Nothing is captured anywhere else, and nothing at all when it is off.
class WheelHook {
public:
    struct Turn {
        int notches{ 0 };       // + away from you (zoom in), - towards you
    };

    WheelHook() = default;
    ~WheelHook();

    WheelHook(const WheelHook&) = delete;
    WheelHook& operator=(const WheelHook&) = delete;

    // "off", "always", "ctrl", "shift", "alt" (only while that key is held).
    // False where the proxy cannot see the mouse (only Windows can).
    bool set_mode(std::string_view mode);
    [[nodiscard]] static bool available();

    [[nodiscard]] std::vector<Turn> take();

private:
    void start();
    void stop();

    std::thread thread_;
    unsigned long thread_id_{ 0 };
    std::string mode_{ "off" };
};
}
