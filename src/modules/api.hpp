#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../map/card.hpp"
#include "../map/coords.hpp"
#include "../packet/packet_variant.hpp"
#include "settings.hpp"

// What the proxy offers to modules. The Lua side of it is the `gtproxy`
// table, see docs/modules.md. Version 1: map clicks, world info,
// reachability, map window, log. Version 2 adds what the proxy's own
// features need: game and server packets, server calls to the game, the
// inventory, items, players, timers, stored data, menu pages and walking.
namespace modules {
struct PlayerView {
    float x{ 0.0f };            // world pixels
    float y{ 0.0f };
    int tile_x{ 0 };
    int tile_y{ 0 };
};

struct WorldView {
    bool connected{ false };
    bool in_world{ false };
    std::string name;
    std::string previous;
    std::vector<std::string> recent;
    int width{ 0 };
    int height{ 0 };
    std::int32_t net_id{ -1 };
    std::string real_name;
    std::optional<PlayerView> me;
    std::string server;         // the game server in config.json (for data per server)
    std::uint32_t user_id{ 0 }; // yours, once spawned
    bool official{ false };     // (4) one of Growtopia's own servers
};

// (4) Your character's abilities on your own server (core::handlers::PlayerMods).
struct Abilities {
    float speed{ 1.0f };
    float gravity{ 1.0f };
    float acceleration{ 1.0f };
    float water_speed{ 1.0f };
    bool double_jump{ false };
    bool noclip{ false };
    int range{ 0 };
    bool god{ false };
};

struct AbilitiesView {
    bool allowed{ false };      // false on Growtopia's own servers
    std::string why;            // the decision, with the game server's address
    std::string game_server;    // "host:port" the proxy plays on ("" before connecting)
    bool state_known{ false };  // the server's character state was seen
    bool state_made{ false };   // none came: the usual values were used
    Abilities abilities;
};

struct PlayerInfo {
    std::int32_t net_id{ -1 };
    std::int32_t user_id{ 0 };
    std::string name;           // with colour codes
    std::string country;
    bool invisible{ false };
    bool local{ false };
    PlayerView position;
};

struct ObjectInfo {
    std::uint32_t object_id{ 0 };
    std::uint16_t item_id{ 0 };
    std::uint8_t amount{ 0 };
    float x{ 0.0f };
    float y{ 0.0f };
};

struct TileInfo {
    std::uint16_t foreground{ 0 };
    std::uint16_t background{ 0 };
    std::uint16_t flags{ 0 };
    std::string cell;           // free, solid, platform, dangerous, door
    std::string text;           // "Rock (10) on Cave Background (14) - solid."
    // What the world data says about the block (API 3).
    std::string extra;          // "vending", "lock", "door", "sign" or ""
    std::uint32_t vend_item{ 0 };
    std::int32_t vend_price{ 0 };   // > 0: World Locks per item, < 0: items per World Lock
    std::uint32_t lock_owner{ 0 };
    std::vector<std::uint32_t> lock_access;
    std::string label;          // door / sign text
};

// A vending machine as the world data shows it (API 3). The stock is not in
// the world data: only the machine's dialog tells it.
struct VendingInfo {
    int x{ 0 };
    int y{ 0 };
    std::uint16_t machine{ 0 };     // the machine's item (Vending Machine, DigiVend ...)
    std::uint32_t item{ 0 };        // 0: empty machine
    std::int32_t price{ 0 };        // > 0: World Locks per item, < 0: items per World Lock, 0: not for sale
};

struct LockInfo {
    int x{ 0 };
    int y{ 0 };
    std::uint16_t item{ 0 };
    std::uint32_t owner{ 0 };       // user ID
    std::vector<std::uint32_t> access;
    bool world{ false };            // locks the whole world (not an area lock)
};

struct TileHit {
    int x{ 0 };
    int y{ 0 };
    bool background{ false };
};

struct WorldSummary {
    std::map<std::uint16_t, int> foreground;
    std::map<std::uint16_t, int> background;
};

struct RouteView {
    bool ok{ false };
    std::vector<map::TilePos> tiles;    // from your tile to the target
    std::string reason;
};

// What a module draws on the map window (API 3).
struct Overlay {
    struct Marker {
        int x{ 0 };
        int y{ 0 };
        std::string label;
        std::string color;          // "#rrggbb"
        bool operator==(const Marker&) const = default;
    };
    struct Area {
        int x1{ 0 };
        int y1{ 0 };
        int x2{ 0 };
        int y2{ 0 };
        std::string label;
        std::string color;
        bool operator==(const Area&) const = default;
    };
    std::vector<Marker> markers;
    std::vector<map::TilePos> route;
    std::string route_color;
    std::vector<Area> areas;
    bool operator==(const Overlay&) const = default;
};

struct InventoryItem {
    std::uint16_t id{ 0 };
    std::uint8_t count{ 0 };
    std::uint8_t flags{ 0 };
};

struct InventoryView {
    bool known{ false };
    std::uint32_t slots{ 0 };
    std::vector<InventoryItem> items;   // by item id
};

struct ItemView {
    std::uint32_t id{ 0 };
    std::string name;
    std::string type;           // items.dat type, e.g. "Lock", "Seed"
    std::uint32_t rarity{ 0 };
    std::uint32_t max_amount{ 0 };
    bool untradeable{ false };
};

// The proxy's reachability check (BFS in core::handlers::MapHandler).
struct ReachView {
    int x{ 0 };
    int y{ 0 };
    std::string status;         // reachable, unreachable, blocked, out_of_bounds, no_data
    std::string headline;       // "Цель доступна", "Цель недоступна", "Нет данных"
    std::string reason;         // "в клетке блок", "за пределами карты" or empty
    std::string message;        // headline, ": reason" when there is one
    std::string missing;        // no_data: what is not known yet
    std::optional<map::TilePos> start;
    std::size_t steps{ 0 };
    std::string steps_word;     // "шаг" / "шага" / "шагов"
    std::string tile_text;      // what is on the tile, empty outside the world
    std::string summary;        // one line for the log
};

struct WalkView {
    bool ok{ false };
    std::string message;
    std::optional<map::TilePos> tile;
};

// How the proxy walks (core::handlers::MapHandler::Settings).
struct WalkOptions {
    float speed{ 250.0f };      // px/s
    int step_ms{ 100 };
    bool gravity{ false };
    int jump_height{ 3 };
    int air_jumps{ 1 };
    std::string doors{ "access" };  // access, all, none
};

// A function call to the game (OnNameChanged, OnChangeSkin ...).
struct CallOut {
    std::string name;
    std::int32_t net_id{ -1 };
    std::int32_t delay{ -1 };
    std::vector<packet::variant> args;  // after the name
};

enum class Source {
    Game,       // /name in the game's chat
    Console,    // typed into the proxy's console window
};

class Services {
public:
    virtual ~Services() = default;

    // API 1
    [[nodiscard]] virtual WorldView world() const = 0;
    [[nodiscard]] virtual ReachView reach(map::TilePos tile) = 0;
    // The answer to the map window click being handled. False when no click
    // is being handled (nothing to show it on).
    virtual bool show(const map::Card& card) = 0;
    // Answer to a command, where it was typed.
    virtual void reply(Source source, const std::string& module_id, const std::string& text) = 0;

    // API 2: reading
    [[nodiscard]] virtual std::vector<PlayerInfo> players() const = 0;
    [[nodiscard]] virtual std::vector<ObjectInfo> objects() const = 0;
    [[nodiscard]] virtual std::optional<TileInfo> tile(map::TilePos tile) const = 0;
    [[nodiscard]] virtual InventoryView inventory() const = 0;
    [[nodiscard]] virtual bool items_loaded() const = 0;
    [[nodiscard]] virtual std::optional<ItemView> item(std::uint32_t id) const = 0;
    [[nodiscard]] virtual std::vector<ItemView> find_items(std::string_view query, std::size_t limit) const = 0;
    [[nodiscard]] virtual char prefix() const = 0;

    // API 2: talking to the game and the server
    virtual void game_message(const std::string& text) = 0;
    virtual void game_dialog(const std::string& markup) = 0;
    virtual bool game_call(const CallOut& call) = 0;
    // false when not connected to the game server
    virtual bool server_text(const std::string& text, bool game_message) = 0;

    // API 2: walking (API 3: gravity for this walk only)
    virtual WalkView walk_to(map::TilePos tile, std::optional<bool> gravity = std::nullopt) = 0;
    virtual WalkView walk_stop() = 0;
    [[nodiscard]] virtual bool walking() const = 0;
    [[nodiscard]] virtual WalkOptions walk_options() const = 0;
    // Applies them (clamped) and returns those in force.
    virtual WalkOptions set_walk_options(const WalkOptions& options) = 0;

    // API 2: a setting changed from a module's page, written to
    // config/modules/<id>.json like an edit by hand. Empty when it is in
    // force, else why not (a broken file, or the active profile sets it).
    virtual std::string save_setting(const std::string& module_id, const std::string& key, const SettingValue& value) = 0;

    // API 2: the mouse wheel over the game's window, taken from the game and
    // given to the module ("wheel" event). mode: "off", "always", "ctrl",
    // "shift", "alt" (only while that key is held). False where the proxy
    // cannot see the mouse (only on Windows).
    virtual bool capture_wheel(const std::string& module_id, const std::string& mode) = 0;

    // API 3: the world's machines, locks and blocks.
    [[nodiscard]] virtual std::vector<VendingInfo> vendings() const = 0;
    [[nodiscard]] virtual std::vector<LockInfo> locks() const = 0;
    [[nodiscard]] virtual WorldSummary world_summary() const = 0;
    [[nodiscard]] virtual std::vector<TileHit> find_tiles(std::uint16_t item) const = 0;

    // API 3: ways. walk false: the reachability check's way (4 neighbours,
    // your body fits); true: the way a walk takes (gravity: on foot or not).
    [[nodiscard]] virtual RouteView route(map::TilePos tile, bool walk, std::optional<bool> gravity) = 0;
    // Steps from your tile to every tile, row by row; -1 no way; empty if unknown.
    [[nodiscard]] virtual std::vector<int> distances() = 0;

    // API 3: what the game sends when you use an item on a tile (32 = the
    // wrench: opens a machine's or a lock's dialog). False if not connected.
    virtual bool tile_action(map::TilePos tile, std::uint32_t item) = 0;

    // API 3: the map window. Overlays (markers, a route, areas) and tabs
    // (JSON built from a Lua table), per module and id; nullopt removes.
    virtual void set_overlay(const std::string& module_id, const std::string& id, const std::optional<Overlay>& overlay) = 0;
    virtual void set_panel(const std::string& module_id, const std::string& id, const std::optional<std::string>& json) = 0;
    // The next left click on the map goes to this module ("pick" event)
    // instead of walking; "" cancels.
    virtual void request_pick(const std::string& module_id, const std::string& label) = 0;
    // A PNG of the map as the window shows it; the file's path, or "" and why.
    virtual std::string render_png(std::string& error) = 0;

    // API 4
    [[nodiscard]] virtual AbilitiesView abilities() const = 0;
    virtual AbilitiesView set_abilities(const Abilities& abilities) = 0;
    // A key held down now: "alt", "ctrl", "shift" (Windows only; false elsewhere).
    [[nodiscard]] virtual bool key_down(std::string_view key) const = 0;
    // The map window is switched on (config.json map.enabled).
    [[nodiscard]] virtual bool map_window() const = 0;

    // API 5: a packet of a module's own making, sent as it is. to_game:
    // to your game (as if the server sent it), else to the game server.
    virtual bool send_raw(bool to_game, std::span<const std::byte> data) = 0;
};
}
