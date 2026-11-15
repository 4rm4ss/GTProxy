#include "grid.hpp"

#include <algorithm>
#include <array>
#include <utility>

#include "../item/item_database.hpp"
#include "../world/tile_map.hpp"

namespace map {
namespace {
// Locks that cover an area (their tiles are marked Locked); any other lock
// (World Lock, Diamond Lock ...) covers the whole world.
constexpr std::array<std::uint16_t, 4> AREA_LOCKS{ 202, 204, 206, 4994 };

const world::tile_extra::Lock* lock_data(const world::Tile& tile)
{
    return tile.extra.get_as<world::tile_extra::Lock>();
}

// The lock that decides who may use a tile; nullptr when nothing locks it.
const world::Tile* governing_lock(const world::Tile& tile, const std::vector<world::Tile>& tiles, const world::Tile* world_lock)
{
    if (world::has_flag(tile.flag, world::TileFlag::Locked)) {
        return tile.lock_parent_tile < tiles.size() ? &tiles[tile.lock_parent_tile] : &tile;
    }
    return world_lock;
}

bool may_enter(const world::Tile& door, const std::vector<world::Tile>& tiles, const world::Tile* world_lock, const std::uint32_t user_id)
{
    if (world::has_flag(door.flag, world::TileFlag::Public)) {
        return true;
    }
    const auto* lock{ governing_lock(door, tiles, world_lock) };
    if (!lock) {
        return true;
    }
    const auto* data{ lock_data(*lock) };
    if (!data || user_id == 0) {
        return false;   // locked, and the proxy cannot tell whether you have access
    }
    return data->owner_id == user_id || std::ranges::find(data->accesses, user_id) != data->accesses.end();
}
}

std::string_view describe(const Cell cell)
{
    switch (cell) {
    case Cell::Open: return "free";
    case Cell::Solid: return "solid";
    case Cell::Platform: return "platform";
    case Cell::Hazard: return "dangerous";
    case Cell::Door: return "door";
    case Cell::OpenDoor: return "door, open to you";
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

Grid Grid::from_tile_map(const WorldTileMap& tile_map, const Doors& doors)
{
    const auto& tiles{ tile_map.get_tiles() };
    const auto& items{ item::ItemDatabase::instance() };

    const world::Tile* world_lock{ nullptr };
    for (const auto& tile : tiles) {
        if (lock_data(tile) && std::ranges::find(AREA_LOCKS, tile.foreground) == AREA_LOCKS.end()) {
            world_lock = &tile;
            break;
        }
    }

    std::vector<Cell> cells;
    cells.reserve(tiles.size());
    for (const auto& tile : tiles) {
        auto cell{ classify(tile) };
        if (cell == Cell::Door && doors.walk_through) {
            const auto* info{ items.get_item(tile.foreground) };
            const bool main_door{ info && info->item_type == item::ItemType::Door };
            if (!main_door && may_enter(tile, tiles, world_lock, doors.user_id)) {
                cell = Cell::OpenDoor;
            }
        }
        cells.push_back(cell);
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
