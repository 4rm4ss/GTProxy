#pragma once
#include <cstdint>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include "coords.hpp"
#include "../item/item_info.hpp"
#include "../world/tile.hpp"

class WorldTileMap;

namespace map {
// How an avatar can move through one tile. Only the foreground counts:
// backgrounds never collide.
enum class Cell : std::uint8_t {
    Open,       // nothing to collide with
    Solid,      // blocks you; you can stand on it
    Platform,   // you can pass through it and stand on it (jump-through)
    Hazard,     // lava, spikes and the like: never walked through
    Door,       // a door you may not enter, or the main door (EXIT): only as a destination
    OpenDoor,   // a door or portal open to you: walked through like a free tile
};

[[nodiscard]] constexpr bool is_passable(const Cell cell)
{
    return cell == Cell::Open || cell == Cell::Platform || cell == Cell::OpenDoor;
}

[[nodiscard]] constexpr bool can_stand_on(const Cell cell)
{
    return cell == Cell::Solid || cell == Cell::Platform;
}

[[nodiscard]] std::string_view describe(Cell cell);

// Classifies a foreground block from its items.dat entry. Without one (no
// items.dat, or an item newer than it) any block counts as solid.
[[nodiscard]] Cell classify(std::uint16_t foreground, world::TileFlag flags, const item::ItemInfo* info);
// Same, looking the item up in the loaded items.dat.
[[nodiscard]] Cell classify(const world::Tile& tile);

// Which doors you may walk through. A door is open to you when it is public,
// when nothing locks it (no lock in the world), or when you own the lock
// that covers it or are on its access list - as in the game. A door with a
// destination still takes you there when you walk in; the main door (EXIT)
// is never walked through.
struct Doors {
    bool walk_through{ true };      // config.json map.walk_through_doors
    std::uint32_t user_id{ 0 };     // yours (OnSpawn userID); 0 = not known yet
};

// Walkability of a whole world, row by row.
class Grid {
public:
    Grid() = default;
    // cells must hold size.x * size.y entries; anything else gives an empty grid.
    Grid(glm::ivec2 size, std::vector<Cell> cells);

    [[nodiscard]] static Grid from_tile_map(const WorldTileMap& tile_map, const Doors& doors = {});

    [[nodiscard]] const glm::ivec2& size() const { return size_; }
    [[nodiscard]] bool empty() const { return cells_.empty(); }
    [[nodiscard]] bool contains(const TilePos tile) const { return in_bounds(tile, size_); }
    // Outside the world counts as solid (worlds end in bedrock anyway).
    [[nodiscard]] Cell at(TilePos tile) const;
    [[nodiscard]] const std::vector<Cell>& cells() const { return cells_; }

private:
    glm::ivec2 size_{ 0, 0 };
    std::vector<Cell> cells_;
};
}
