#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../map/card.hpp"
#include "../map/coords.hpp"
#include "../packet/packet_variant.hpp"

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

    // API 2: walking
    virtual WalkView walk_to(map::TilePos tile) = 0;
    virtual WalkView walk_stop() = 0;
    [[nodiscard]] virtual bool walking() const = 0;
};
}
