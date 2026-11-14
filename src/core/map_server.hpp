#pragma once
#include <optional>
#include <string>
#include <thread>

#include <httplib.h>

#include "config.hpp"
#include "../map/bridge.hpp"

namespace core {
// Serves the map window on http://127.0.0.1:<port>/ (this computer only):
// the page itself, the map state as JSON, and the window's clicks, which it
// hands to the main loop through map::Bridge.
class MapServer {
public:
    MapServer(const Config::MapConfig& config, map::Bridge& bridge);
    ~MapServer();

    MapServer(const MapServer&) = delete;
    MapServer& operator=(const MapServer&) = delete;

    // The window's address, when the server is running.
    [[nodiscard]] std::optional<std::string> url() const;

private:
    void setup_routes();
    // Only pages opened from this computer may use the map: the Host header
    // stops other web sites reaching it through DNS tricks, the custom header
    // on clicks stops other pages' forms and scripts from sending them.
    [[nodiscard]] bool allowed_host(const httplib::Request& req) const;

    map::Bridge& bridge_;
    int port_;
    bool running_{ false };
    httplib::Server server_;
    std::thread thread_;
};
}
