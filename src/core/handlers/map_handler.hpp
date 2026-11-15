#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
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
#include "../../map/render.hpp"
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

    // How walks are made. Set from config.json "map" at start; modules/walk
    // changes them at run time (its settings and the Walking page of /proxy).
    struct Settings {
        float speed{ 250.0f };          // world pixels per second (32 per tile)
        int step_ms{ 100 };             // a position is sent this often
        bool gravity{ false };          // only paths you could run and jump along
        int jump_height{ 3 };           // tiles one jump rises (gravity)
        int air_jumps{ 1 };             // jumps before landing again (gravity)
        map::DoorMode doors{ map::DoorMode::Access };
    };
    static constexpr float MIN_SPEED{ 16.0f };
    static constexpr float MAX_SPEED{ 10000.0f };
    static constexpr int MIN_STEP_MS{ 10 };
    static constexpr int MAX_STEP_MS{ 1000 };
    static constexpr int MAX_JUMP_HEIGHT{ 200 };
    static constexpr int MAX_AIR_JUMPS{ 1000 };

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
    // Walk to a tile (a block: to the free tile next to it). gravity: only
    // a way you could run and jump along (instead of the setting).
    Result walk_to(map::TilePos tile, std::optional<bool> gravity = std::nullopt);

    // Ways without walking. reach_route: the reachability check's way (4
    // neighbours, your body fits, never between two corners); walk_route:
    // the way a walk would take (gravity as for walk_to).
    struct Route {
        bool ok{ false };
        std::vector<map::TilePos> tiles;    // from your tile to the target
        std::string reason;                 // why there is none
    };
    [[nodiscard]] Route reach_route(map::TilePos target);
    [[nodiscard]] Route walk_route(map::TilePos target, std::optional<bool> gravity);
    // Steps from your tile to every tile (row by row), -1 where there is no
    // way; empty while the world or your position is unknown.
    [[nodiscard]] std::vector<int> reach_distances();
    // The map as a PNG (map/render.hpp), with the modules' marks.
    [[nodiscard]] std::vector<std::uint8_t> render_png(const map::RenderInput& input);

    // The modules' part of the map window (tabs, overlays: JSON) and a module
    // waiting for a click on a tile ("" none).
    void set_module_view(std::string json);
    void set_pick(std::string label);
    // Buttons and rows of the modules' tabs; returns the answer (JSON).
    using PanelHook = std::function<std::string(const std::map<std::string, std::string>& params)>;
    void set_panel_hook(PanelHook hook) { panel_hook_ = std::move(hook); }
    // What is on a tile.
    [[nodiscard]] Result inspect(map::TilePos tile) const;
    Result stop();

    [[nodiscard]] bool walking() const { return walk_.has_value(); }

    [[nodiscard]] Settings settings() const;
    // Applies the settings (clamped to the limits above; a walk in progress
    // keeps its speed) and returns those in force.
    Settings set_settings(const Settings& settings);
    // Why there is no way to a tile when doors are the reason: which door
    // or entrance blocks it and why it is closed to you. Empty otherwise.
    [[nodiscard]] std::string blocked_by_door(map::TilePos start, map::TilePos goal);
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
        map::PathOptions options;
    };

    [[nodiscard]] Result plan_walk(map::TilePos tile, const map::PathOptions& options);
    [[nodiscard]] map::PathOptions options_with(std::optional<bool> gravity) const;
    // Your avatar's position and body, when known.
    [[nodiscard]] std::optional<std::pair<glm::vec2, map::Body>> me() const;
    [[nodiscard]] Result module_click(map::MouseButton button, map::TilePos tile);
    void log_settings() const;
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
    map::Doors doors_;
    std::uint64_t published_tiles_{ 0 };
    map::Grid grid_;

    std::optional<Walk> walk_;
    std::optional<map::TilePos> last_target_;
    // The game's last state packet: the model for the ones sent while walking.
    std::optional<packet::GameUpdatePacket> model_;
    ClickHook click_hook_;
    WalkEndHook walk_end_hook_;
    PanelHook panel_hook_;
    std::uint64_t modules_rev_{ 0 };
    std::string pick_;
    std::string status_;
    std::chrono::steady_clock::time_point next_publish_{};
};
}
