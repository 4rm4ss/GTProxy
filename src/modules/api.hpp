#pragma once
#include <cstddef>
#include <optional>
#include <string>

#include "../map/card.hpp"
#include "../map/coords.hpp"

// What the proxy offers to modules (module API version 1). The Lua side of
// it is the `gtproxy` table, see docs/modules.md.
namespace modules {
struct PlayerView {
    float x{ 0.0f };            // world pixels
    float y{ 0.0f };
    int tile_x{ 0 };
    int tile_y{ 0 };
};

struct WorldView {
    bool in_world{ false };
    std::string name;
    int width{ 0 };
    int height{ 0 };
    std::optional<PlayerView> me;
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

enum class Source {
    Game,       // /name in the game's chat
    Console,    // typed into the proxy's console window
};

class Services {
public:
    virtual ~Services() = default;

    [[nodiscard]] virtual WorldView world() const = 0;
    [[nodiscard]] virtual ReachView reach(map::TilePos tile) = 0;
    // The answer to the map window click being handled. False when no click
    // is being handled (nothing to show it on).
    virtual bool show(const map::Card& card) = 0;
    // Answer to a command, where it was typed.
    virtual void reply(Source source, const std::string& module_id, const std::string& text) = 0;
};
}
