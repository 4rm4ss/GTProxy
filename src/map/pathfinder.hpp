#pragma once
#include <cstddef>
#include <expected>
#include <string_view>
#include <vector>

#include "coords.hpp"
#include "grid.hpp"

namespace map {
struct PathOptions {
    // false: move through any open tile in 8 directions, ignoring gravity.
    // true:  on foot. After leaving the ground you rise at most jump_height
    //        tiles (sideways steps count too), then you can only fall,
    //        straight or diagonally, until you land on something.
    bool gravity{ false };
    int jump_height{ 3 };
    // Jumps you can make before landing again (wings, double jump ...):
    // each one adds jump_height to how far you can rise. 1 = a single jump.
    int air_jumps{ 1 };
    // Search limit, so a far unreachable click can never stall the proxy.
    std::size_t max_expanded{ 1'000'000 };
};

enum class PathError {
    EmptyWorld,
    StartOutside,
    GoalOutside,
    GoalBlocked,
    NoPath,
};

[[nodiscard]] std::string_view describe(PathError error);

// Tiles from the start to the goal, both included. Consecutive tiles are
// neighbours (including diagonals).
using Path = std::vector<TilePos>;

// A tile a walk may end in: an open tile or a door.
[[nodiscard]] constexpr bool can_end_in(const Cell cell)
{
    return is_passable(cell) || cell == Cell::Door;
}

// A* over the grid. The start tile is never checked (you are already there);
// doors open to you are walked through, other doors only entered as the goal.
[[nodiscard]] std::expected<Path, PathError> find_path(
    const Grid& grid,
    TilePos start,
    TilePos goal,
    const PathOptions& options = {}
);

struct Destination {
    TilePos tile;
    Path path;
    bool next_to_goal{ false }; // the goal is a block: you stop beside it
};

// Like find_path, but a click on a block leads to the reachable tile next to
// it that is closest to you, as you would walk up to a block to punch it.
[[nodiscard]] std::expected<Destination, PathError> find_destination(
    const Grid& grid,
    TilePos start,
    TilePos goal,
    const PathOptions& options = {}
);
}
