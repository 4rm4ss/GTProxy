#include "grid.hpp"

#include <algorithm>
#include <array>
#include <string>
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

// Who may use a door or entrance: everyone (empty), or why not.
std::string closed_reason(const world::Tile& door, const std::vector<world::Tile>& tiles, const world::Tile* world_lock, const std::uint32_t user_id)
{
    if (world::has_flag(door.flag, world::TileFlag::Public)) {
        return {};
    }

    // A VIP Entrance has its own list.
    if (const auto* vip{ door.extra.get_as<world::tile_extra::VipEntrance>() }) {
        if (user_id != 0 && (vip->owner_id == user_id || std::ranges::find(vip->accesses, user_id) != vip->accesses.end())) {
            return {};
        }
    }

    const auto* lock{ governing_lock(door, tiles, world_lock) };
    if (!lock) {
        return {};
    }
    const auto* data{ lock_data(*lock) };
    if (!data) {
        return "it is locked and the lock is not known";
    }
    if (user_id == 0) {
        return "it is locked and your user ID is not known yet";
    }
    if (data->owner_id == user_id || std::ranges::find(data->accesses, user_id) != data->accesses.end()) {
        return {};
    }
    return "it is locked by user " + std::to_string(data->owner_id) + " (you are " + std::to_string(user_id) + ")";
}

bool is_entrance(const item::ItemInfo& info)
{
    switch (info.collision_type) {
    case item::CollisionType::Gateway:
    case item::CollisionType::VipDoor:
    case item::CollisionType::Adventure:
    case item::CollisionType::Faction:
    case item::CollisionType::Guild:
    case item::CollisionType::FriendEntrance:
        return true;
    default:
        return false;
    }
}

bool is_door(const item::ItemInfo& info)
{
    return info.item_type == item::ItemType::UserDoor || info.item_type == item::ItemType::Portal
        || info.item_type == item::ItemType::Door;
}

const world::Tile* find_world_lock(const std::vector<world::Tile>& tiles)
{
    for (const auto& tile : tiles) {
        if (lock_data(tile) && std::ranges::find(AREA_LOCKS, tile.foreground) == AREA_LOCKS.end()) {
            return &tile;
        }
    }
    return nullptr;
}
}

std::string_view describe(const DoorMode mode)
{
    switch (mode) {
    case DoorMode::Access: return "access";
    case DoorMode::All: return "all";
    case DoorMode::None: return "none";
    }
    return "access";
}

std::optional<DoorMode> door_mode(const std::string_view text)
{
    for (const auto mode : { DoorMode::Access, DoorMode::All, DoorMode::None }) {
        if (text == describe(mode)) {
            return mode;
        }
    }
    return std::nullopt;
}

bool is_door_or_entrance(const item::ItemInfo& info)
{
    return is_door(info) || is_entrance(info);
}

std::string closed_because(const WorldTileMap& tile_map, const TilePos tile, const Doors& doors)
{
    const auto& tiles{ tile_map.get_tiles() };
    const auto& size{ tile_map.get_size() };
    if (!in_bounds(tile, size) || tile_index(tile, size.x) >= tiles.size()) {
        return {};
    }
    const auto& data{ tiles[tile_index(tile, size.x)] };
    const auto* info{ item::ItemDatabase::instance().get_item(data.foreground) };
    if (!info || !is_door_or_entrance(*info)) {
        return {};
    }
    if (info->item_type == item::ItemType::Door) {
        return "it is the world's main door (EXIT)";
    }
    if (doors.mode == DoorMode::None) {
        return "doors are switched off for walking (doors: none)";
    }
    if (doors.mode == DoorMode::All) {
        return {};
    }
    return closed_reason(data, tiles, find_world_lock(tiles), doors.user_id);
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
    const auto* world_lock{ find_world_lock(tiles) };

    std::vector<Cell> cells;
    cells.reserve(tiles.size());
    for (const auto& tile : tiles) {
        auto cell{ classify(tile) };
        const auto* info{ tile.foreground != 0 ? items.get_item(tile.foreground) : nullptr };
        // Doors and entrances other than the main door, per the door mode.
        if (info && is_door_or_entrance(*info) && info->item_type != item::ItemType::Door && doors.mode != DoorMode::None) {
            if (doors.mode == DoorMode::All || closed_reason(tile, tiles, world_lock, doors.user_id).empty()) {
                cell = cell == Cell::Open ? Cell::Open : Cell::OpenDoor;
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
