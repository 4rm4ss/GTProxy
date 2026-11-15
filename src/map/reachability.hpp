#pragma once
#include <cstddef>
#include <optional>
#include <vector>
#include <string_view>

#include <glm/glm.hpp>

#include "coords.hpp"
#include "grid.hpp"

// "Can I get to that tile from where I stand?" - a breadth-first search over
// the four neighbours of each tile (no diagonal steps, so no slipping between
// two blocks that only touch at a corner). It ignores gravity: it answers
// whether a free way exists, not how to jump along it.
namespace map {
// The avatar's collision box relative to its position, in world pixels
// ("colrect|x|y|w|h" in OnSpawn; 0|0|20|30 for a normal avatar).
struct Body {
    glm::ivec2 offset{ 0, 0 };
    glm::ivec2 size{ 20, 30 };

    // Uses the server's colrect; the normal avatar size if it is missing.
    [[nodiscard]] static Body from_colrect(const glm::ivec4& colrect);
};

// Tiles the body covers while standing in 'tile' (position = the tile's
// top-left corner, as the game places you). A 20x30 body covers one tile.
struct TileSpan {
    TilePos first;
    TilePos last;
};
[[nodiscard]] TileSpan covered_tiles(TilePos tile, const Body& body);

// The tile you are in: the one under the centre of your body.
[[nodiscard]] TilePos body_tile(glm::vec2 position, const Body& body);

enum class Reach {
    Reachable,      // a way through free tiles exists
    Unreachable,    // the target is free, but closed off from you
    TargetBlocked,  // a block (or something dangerous) is in the target tile
    OutOfBounds,    // the target is outside the world
    NoData,         // no world or no position yet: nothing can be said
};

// "Цель доступна", "Цель недоступна", "Нет данных".
[[nodiscard]] std::string_view headline(Reach reach);
// Why a target is rejected, for TargetBlocked / OutOfBounds; empty otherwise.
[[nodiscard]] std::string_view reason(Reach reach);
// "шаг" / "шага" / "шагов" for a number of steps.
[[nodiscard]] std::string_view steps_word(std::size_t steps);

// "reachable", "unreachable", "blocked", "out_of_bounds", "no_data".
[[nodiscard]] std::string_view code(Reach reach);

struct ReachResult {
    Reach status{ Reach::NoData };
    std::optional<TilePos> start;   // your tile, when known
    std::size_t steps{ 0 };         // shortest way in tiles, when reachable
};

// position: your avatar's position in world pixels, nothing if unknown.
// Doors open to you (Cell::OpenDoor) are walked through; other doors are only
// allowed as the target.
[[nodiscard]] ReachResult check_reach(
    const Grid& grid,
    std::optional<glm::vec2> position,
    const Body& body,
    TilePos target
);

// The same search from where you stand to every tile: steps to each tile
// (row by row), -1 where there is no way. Closed doors are not entered.
[[nodiscard]] std::vector<int> reach_distances(const Grid& grid, glm::vec2 position, const Body& body);

// The shortest way itself, from your tile to the target; empty when there is
// none (or the target is blocked / outside the world).
[[nodiscard]] std::vector<TilePos> reach_path(const Grid& grid, glm::vec2 position, const Body& body, TilePos target);
}
