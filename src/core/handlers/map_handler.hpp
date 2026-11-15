#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "session_handler.hpp"
#include "../config.hpp"
#include "../../event/event.hpp"
#include "../../map/bridge.hpp"
#include "../../map/card.hpp"
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
// The map window's clicks and the walking engine modules use.
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
        std::string action;             // "walk", "card", "right_click", "inspect" or "stop"
        std::optional<map::TilePos> tile;
        std::string message;            // "card": its title
        std::string reach;              // "card": the status tag a module gave
        std::string detail;             // "card": its text
        std::string kind{ "none" };     // "card": ok / bad / none
        bool marker{ true };            // mark the tile in the window
    };

    // The reachability check's answer with what was used for it.
    struct ReachReport {
        map::TilePos target;
        map::ReachResult result;
        std::optional<glm::vec2> position;  // your avatar, world pixels
        map::Body body;
        std::string missing;                // NoData: what is not known yet
        std::string tile_text;              // what is on the target tile

        // "Цель недоступна: в клетке блок"
        [[nodiscard]] std::string message() const;
        // "start tile 5, 4 at 160, 128 px (body 20x30), target 0, 4 -> Цель доступна (5 steps)"
        [[nodiscard]] std::string summary() const;
    };

    // A click in the map window is answered by the modules: handled tells
    // whether any module listens, card is what one of them showed.
    struct ClickAnswer {
        bool handled{ false };
        std::optional<map::Card> card;
    };
    using ClickHook = std::function<ClickAnswer(map::MouseButton button, map::TilePos tile)>;
    // A walk ended: why, and whether to tell the player (arrived, stopped by
    // the server or a blocked way; not when you left the world or pressed Stop).
    using WalkEndHook = std::function<void(const std::string& message, bool notify)>;

    MapHandler(
        event::Dispatcher& dispatcher,
        network::Server& server,
        network::Client& client,
        SessionHandler& session,
        const Config::MapConfig& config,
        map::Bridge& bridge,
        std::optional<std::string> window_url
    );

    MapHandler(const MapHandler&) = delete;
    MapHandler& operator=(const MapHandler&) = delete;
    ~MapHandler() = default;

    // A click in the map window: cursor -> tile, then the modules answer it
    // (walk on the left button, reachability on the right one).
    Result handle_click(const map::Click& click);
    void set_click_hook(ClickHook hook) { click_hook_ = std::move(hook); }
    void set_walk_end_hook(WalkEndHook hook) { walk_end_hook_ = std::move(hook); }
    // Can you get to the tile from where you stand: BFS over the 4
    // neighbours of each tile; your avatar's collision box has to fit in
    // every tile on the way. "Цель доступна", "Цель недоступна" or "Нет
    // данных" (no world, items.dat or position yet). Only reports and logs
    // nothing: the reachability module decides what is shown and logged.
    [[nodiscard]] ReachReport check_reach(map::TilePos target);
    // Walk to a tile (a block: to the free tile next to it).
    Result walk_to(map::TilePos tile);
    // What is on a tile.
    [[nodiscard]] Result inspect(map::TilePos tile) const;
    Result stop();

    [[nodiscard]] bool walking() const { return walk_.has_value(); }
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
    [[nodiscard]] Result module_click(map::MouseButton button, map::TilePos tile);
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
    void on_set_pos(const event::Event& event);
    void on_world_change();

    event::Dispatcher& dispatcher_;
    network::Server& server_;
    network::Client& client_;
    SessionHandler& session_;
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
    ClickHook click_hook_;
    WalkEndHook walk_end_hook_;
    std::string status_;
    std::chrono::steady_clock::time_point next_publish_{};
};
}
