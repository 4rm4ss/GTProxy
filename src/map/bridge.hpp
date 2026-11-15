#pragma once
#include <chrono>
#include <cstddef>
#include <deque>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "click.hpp"

namespace map {
// Connects the map window's HTTP thread with the proxy's main loop. World and
// network state are only ever touched on the main thread: the window reads
// snapshots published here, and its clicks wait in a queue until the main
// loop answers them.
class Bridge {
public:
    struct Request {
        enum class Kind { Click, Stop, Panel };

        Kind kind{ Kind::Click };
        Click click;
        std::map<std::string, std::string> params;  // Panel: panel, action, row, value ...
        std::promise<std::string> reply; // JSON
    };

    static constexpr std::size_t MAX_PENDING{ 16 };

    // HTTP thread.
    // Queues a request; the future gets the main loop's JSON answer. Nothing
    // when too many requests are already waiting.
    [[nodiscard]] std::optional<std::future<std::string>> submit(Request::Kind kind, const Click& click = {}, std::map<std::string, std::string> params = {});
    [[nodiscard]] std::shared_ptr<const std::string> state() const;
    [[nodiscard]] std::shared_ptr<const std::string> tiles() const;
    // The modules' tabs and map overlays (see core::ModuleSystem).
    [[nodiscard]] std::shared_ptr<const std::string> modules() const;
    // A window asked for the state: keep publishing it.
    void mark_viewed();

    // Main thread.
    [[nodiscard]] std::vector<Request> take_requests();
    void publish_state(std::string json);
    void publish_tiles(std::string json);
    void publish_modules(std::string json);
    [[nodiscard]] bool viewed_within(std::chrono::steady_clock::duration window) const;

private:
    mutable std::mutex mutex_;
    std::deque<Request> requests_;
    std::shared_ptr<const std::string> state_{ std::make_shared<const std::string>("{}") };
    std::shared_ptr<const std::string> tiles_{ std::make_shared<const std::string>("{}") };
    std::shared_ptr<const std::string> modules_{ std::make_shared<const std::string>("{\"panels\":[],\"overlays\":[]}") };
    std::chrono::steady_clock::time_point last_view_{};
    bool viewed_{ false };
};
}
