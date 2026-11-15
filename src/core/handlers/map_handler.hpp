#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "feature_handler.hpp"
#include "../config.hpp"
#include "../../event/event.hpp"
#include "../../map/bridge.hpp"
#include "../../map/click.hpp"
#include "../../map/grid.hpp"
#include "../../map/movement.hpp"
#include "../../map/pathfinder.hpp"
#include "../../map/reachability.hpp"
#include "../../network/client.hpp"
#include "../../network/server.hpp"
#include "../../packet/packet_types.hpp"
#include "../../packet/payload.hpp"

namespace core::handlers {
// Click handling behind the map window, /goto and /clickwalk.
//
// A click becomes a tile (cursor -> tile with the window's view), the tile
// becomes a path (A* over the world's blocks), and the path is walked: the
// proxy sends the server the same state packets the game sends while you
// run - a position every step_interval_ms at walk_speed, never more than a
// fraction of a tile apart - and moves your avatar in the game with OnSetPos
// so it shows where you are. While walking, the game's own state packets are
// held back so the two never disagree.
//
// The Growtopia server checks movement with rules this proxy does not know;
// walking at a normal speed through free tiles is the safe choice, and the
// walk stops as soon as the server puts you somewhere else.
class MapHandler {
public:
    struct Result {
        bool ok{ false };
        std::string action;             // "walk", "reach", "inspect" or "stop"
        std::optional<map::TilePos> tile;
        std::string message;
        std::string reach;              // "reach": map::code() of the result
        std::string detail;             // "reach": start, target, steps, tile
    };

    MapHandler(
        event::Dispatcher& dispatcher,
        network::Server& server,
        network::Client& client,
        FeatureHandler& features,
        const Config::MapConfig& config,
        map::Bridge& bridge,
        std::optional<std::string> window_url
    );

    MapHandler(const MapHandler&) = delete;
    MapHandler& operator=(const MapHandler&) = delete;
    ~MapHandler() = default;

    // A click in the map window: cursor -> tile, then walk (left button) or
    // the reachability check (right button).
    Result handle_click(const map::Click& click);
    // Can you get to the tile from where you stand: BFS over the 4
    // neighbours of each tile, your avatar's collision box has to fit in
    // every tile on the way. "Цель доступна", "Цель недоступна" or "Нет
    // данных" (no world, items.dat or position yet). Only reports; logs your
    // position, the tile and the result. 'source' names who asked (log).
    Result check_reach(map::TilePos target, std::string_view source);
    // Walk to a tile (a block: to the free tile next to it).
    Result walk_to(map::TilePos tile);
    // What is on a tile.
    [[nodiscard]] Result inspect(map::TilePos tile) const;
    Result stop();

    [[nodiscard]] bool walking() const { return walk_.has_value(); }
    [[nodiscard]] bool click_walk() const { return click_walk_; }
    void set_click_walk(bool on);
    // http://127.0.0.1:<port>/ when the window's server runs.
    [[nodiscard]] const std::optional<std::string>& window_url() const { return window_url_; }

    // Called from the main loop.
    void update();

private:
    struct Walk {
        map::Walker walker;
        map::Path path;
        map::TilePos target;
        bool next_to_goal{ false };
        bool facing_left{ false };
        std::chrono::steady_clock::time_point last_step;
        std::chrono::steady_clock::time_point next_step;
    };

    [[nodiscard]] Result plan_walk(map::TilePos tile);
    [[nodiscard]] const map::Grid& grid();
    void mark_tiles_changed();
    void end_walk(const std::string& message, bool tell_game);
    void step_walk(std::chrono::steady_clock::time_point now);
    void check_path();
    void serve_window();
    void publish(std::chrono::steady_clock::time_point now);

    void send_to_server(const packet::Payload& payload) const;
    void send_to_client(const packet::Payload& payload) const;

    void on_state(const event::Event& event);
    void on_tile_change_request(const event::Event& event);
    void on_set_pos(const event::Event& event);
    void on_world_change();

    event::Dispatcher& dispatcher_;
    network::Server& server_;
    network::Client& client_;
    FeatureHandler& features_;
    map::Bridge& bridge_;
    std::optional<std::string> window_url_;
    std::vector<event::ScopedHandle> handles_;

    map::PathOptions path_options_;
    float walk_speed_;
    std::chrono::milliseconds step_interval_;

    std::uint64_t tiles_revision_{ 1 };
    std::uint64_t grid_revision_{ 0 };
    std::uint64_t published_tiles_{ 0 };
    map::Grid grid_;

    std::optional<Walk> walk_;
    std::optional<map::TilePos> last_target_;
    // The game's last state packet: the model for the ones sent while walking.
    std::optional<packet::GameUpdatePacket> model_;
    bool click_walk_{ false };
    std::string status_;
    std::chrono::steady_clock::time_point next_publish_{};
};
}
