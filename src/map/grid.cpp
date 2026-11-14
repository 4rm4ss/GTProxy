#include "grid.hpp"

#include <utility>

#include "../item/item_database.hpp"
#include "../world/tile_map.hpp"

namespace map {
std::string_view describe(const Cell cell)
{
    switch (cell) {
    case Cell::Open: return "free";
    case Cell::Solid: return "solid";
    case Cell::Platform: return "platform";
    case Cell::Hazard: return "dangerous";
    case Cell::Door: return "door";
    }
    return "unknown";
}

Cell classify(const std::uint16_t foreground, const world::TileFlag flags, const item::ItemInfo* info)
{
    if (foreground == 0) {
        return Cell::Open;
    }

    if (!info) {
        return Cell::Solid;
    }

    switch (info->item_type) {
    case item::ItemType::Deadly:
    case item::ItemType::Lava:
    case item::ItemType::Pointy:
        return Cell::Hazard;
    case item::ItemType::Door:
    case item::ItemType::UserDoor:
    case item::ItemType::Portal:
        return Cell::Door;
    default:
        break;
    }

    // "Open" is the on/off state of switchable blocks.
    const bool switched_on{ world::has_flag(flags, world::TileFlag::Open) };

    switch (info->collision_type) {
    case item::CollisionType::None:
        return Cell::Open;
    case item::CollisionType::Full:
        return Cell::Solid;
    case item::CollisionType::JumpThrough:
    case item::CollisionType::JumpDown:
    case item::CollisionType::Cloud:
        return Cell::Platform;
    case item::CollisionType::CollideIfOff:
        return switched_on ? Cell::Open : Cell::Solid;
    case item::CollisionType::CollideIfOn:
        return switched_on ? Cell::Solid : Cell::Open;
    // Entrances: the proxy cannot know whether you have access, so only
    // public ones are walked through.
    case item::CollisionType::Gateway:
    case item::CollisionType::VipDoor:
    case item::CollisionType::Adventure:
    case item::CollisionType::Faction:
    case item::CollisionType::Guild:
    case item::CollisionType::FriendEntrance:
        return world::has_flag(flags, world::TileFlag::Public) ? Cell::Open : Cell::Solid;
    // One-way blocks depend on the direction you come from: avoid them.
    case item::CollisionType::OneWay:
    default:
        return Cell::Solid;
    }
}

Cell classify(const world::Tile& tile)
{
    if (tile.foreground == 0) {
        return Cell::Open;
    }

    return classify(tile.foreground, tile.flag, item::ItemDatabase::instance().get_item(tile.foreground));
}

Grid::Grid(const glm::ivec2 size, std::vector<Cell> cells)
{
    if (size.x <= 0 || size.y <= 0) {
        return;
    }

    if (cells.size() != static_cast<std::size_t>(size.x) * static_cast<std::size_t>(size.y)) {
        return;
    }

    size_ = size;
    cells_ = std::move(cells);
}

Grid Grid::from_tile_map(const WorldTileMap& tile_map)
{
    const auto& tiles{ tile_map.get_tiles() };
    std::vector<Cell> cells;
    cells.reserve(tiles.size());
    for (const auto& tile : tiles) {
        cells.push_back(classify(tile));
    }

    return Grid{ tile_map.get_size(), std::move(cells) };
}

Cell Grid::at(const TilePos tile) const
{
    if (!contains(tile)) {
        return Cell::Solid;
    }

    return cells_[tile_index(tile, size_.x)];
}
}
