#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "coords.hpp"
#include "grid.hpp"

namespace world {
class World;
}

// What the map window shows, as JSON. Built on the main thread and handed to
// the window through map::Bridge.
namespace map {
// Colour a block is drawn with ("#rrggbb"): the colour of its seed in
// items.dat, which matches the block (brown dirt, grey rock, red lava ...).
[[nodiscard]] std::string item_color(std::uint16_t item_id);

// The world's tiles; only sent again when they change (revision).
[[nodiscard]] std::string tiles_json(std::uint64_t revision, const world::World& world, const Grid& grid);

struct WalkView {
    bool walking{ false };
    std::vector<TilePos> path;          // what is left of the current walk
    std::optional<TilePos> target;      // where the last walk goes / went
};

struct StateView {
    bool connected{ false };
    bool in_world{ false };
    std::string world_name;
    std::uint64_t tiles_revision{ 0 };
    WalkView walk;
    bool gravity{ false };
    bool click_walk{ false };
    std::string status;                 // last result, shown in the window
};

// Players, dropped items and the current walk; sent several times a second.
[[nodiscard]] std::string state_json(const StateView& view, const world::World& world);

// Answer to a click or a button in the window.
[[nodiscard]] std::string result_json(
    bool ok,
    std::string_view action,
    std::optional<TilePos> tile,
    std::string_view message,
    std::string_view reach = {},
    std::string_view detail = {}
);
}
