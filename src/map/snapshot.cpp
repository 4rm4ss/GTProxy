#include "snapshot.hpp"

#include <map>
#include <unordered_set>

#include <fmt/format.h>
#include <glaze/glaze.hpp>
#include <spdlog/spdlog.h>

#include "../core/features.hpp"
#include "../item/item_database.hpp"
#include "../world/world.hpp"

namespace map {
namespace {
struct PointJson {
    int x{ 0 };
    int y{ 0 };
};

struct ItemJson {
    std::string name;
    std::string color;
};

struct TilesJson {
    std::uint64_t rev{ 0 };
    std::string world;
    int width{ 0 };
    int height{ 0 };
    std::vector<std::uint16_t> fg;
    std::vector<std::uint16_t> bg;
    // map::Cell of each tile: 0 open, 1 solid, 2 platform, 3 hazard, 4 door
    std::vector<std::uint8_t> cell;
    std::map<std::string, ItemJson> items;
};

struct PlayerJson {
    std::int32_t id{ 0 };
    std::string name;
    float x{ 0.0f };
    float y{ 0.0f };
    int tx{ 0 };
    int ty{ 0 };
    bool me{ false };
};

struct ObjectJson {
    std::uint16_t item{ 0 };
    std::uint8_t amount{ 0 };
    float x{ 0.0f };
    float y{ 0.0f };
};

struct ResultJson {
    bool ok{ false };
    std::string action;
    bool has_tile{ false };
    PointJson tile;
    std::string message;
    std::string reach;      // right click: map::code(Reach)
    std::string detail;
};

struct StateJson {
    bool connected{ false };
    bool in_world{ false };
    std::string world;
    std::uint64_t tiles_rev{ 0 };
    int width{ 0 };
    int height{ 0 };
    std::vector<PlayerJson> players;
    std::vector<ObjectJson> objects;
    std::map<std::string, std::string> object_names;
    bool walking{ false };
    std::vector<PointJson> path;
    bool has_target{ false };
    PointJson target;
    bool gravity{ false };
    bool click_walk{ false };
    std::string status;
};

std::string hex(const std::uint32_t r, const std::uint32_t g, const std::uint32_t b)
{
    return fmt::format("#{:02x}{:02x}{:02x}", r & 0xff, g & 0xff, b & 0xff);
}

std::string item_name(const std::uint16_t item_id)
{
    if (const auto* info{ item::ItemDatabase::instance().get_item(item_id) }) {
        return info->item_name;
    }
    return fmt::format("Item {}", item_id);
}

template <typename T>
std::string to_json(const T& value)
{
    std::string out;
    if (const auto ec{ glz::write_json(value, out) }) {
        spdlog::warn("Map window: could not build JSON ({})", glz::format_error(ec, out));
        return "{}";
    }
    return out;
}
}

std::string item_color(const std::uint16_t item_id)
{
    const auto& items{ item::ItemDatabase::instance() };
    const auto* info{ items.get_item(item_id) };

    if (info && info->item_type == item::ItemType::Bedrock) {
        return "#3b3b45";
    }

    // Blocks are followed by their seed in items.dat.
    const auto* seed{ info && info->item_type == item::ItemType::Seed ? info : items.get_item(item_id + 1u) };
    if (seed && seed->item_type == item::ItemType::Seed) {
        return hex(seed->seed_color.r, seed->seed_color.g, seed->seed_color.b);
    }

    // Without items.dat: a stable colour per item.
    const std::uint32_t h{ (static_cast<std::uint32_t>(item_id) + 1u) * 2654435761u };
    return hex(96 + (h >> 8) % 128, 96 + (h >> 16) % 128, 96 + (h >> 24) % 128);
}

std::string tiles_json(const std::uint64_t revision, const world::World& world, const Grid& grid)
{
    const auto& tile_map{ world.get_tile_map() };
    const auto& tiles{ tile_map.get_tiles() };

    TilesJson doc{};
    doc.rev = revision;
    doc.world = world.get_name();

    const auto size{ tile_map.get_size() };
    const bool complete{
        size.x > 0 && size.y > 0
        && tiles.size() == static_cast<std::size_t>(size.x) * static_cast<std::size_t>(size.y)
        && grid.size() == size
    };
    if (!complete) {
        return to_json(doc);
    }

    doc.width = size.x;
    doc.height = size.y;
    doc.fg.reserve(tiles.size());
    doc.bg.reserve(tiles.size());
    doc.cell.reserve(tiles.size());

    std::unordered_set<std::uint16_t> used;
    for (std::size_t i{ 0 }; i < tiles.size(); ++i) {
        doc.fg.push_back(tiles[i].foreground);
        doc.bg.push_back(tiles[i].background);
        doc.cell.push_back(static_cast<std::uint8_t>(grid.cells()[i]));
        used.insert(tiles[i].foreground);
        used.insert(tiles[i].background);
    }

    used.erase(0);
    for (const auto id : used) {
        doc.items.emplace(std::to_string(id), ItemJson{ item_name(id), item_color(id) });
    }

    return to_json(doc);
}

std::string state_json(const StateView& view, const world::World& world)
{
    const auto size{ world.get_tile_map().get_size() };

    StateJson doc{};
    doc.connected = view.connected;
    doc.in_world = view.in_world;
    doc.world = view.world_name;
    doc.tiles_rev = view.tiles_revision;
    doc.width = size.x;
    doc.height = size.y;
    doc.walking = view.walk.walking;
    doc.gravity = view.gravity;
    doc.click_walk = view.click_walk;
    doc.status = view.status;

    if (view.in_world) {
        for (const auto& [net_id, player] : world.get_players()) {
            // The game does not show invisible players either.
            if (!player || (player->invisible() != 0 && !player->is_local())) {
                continue;
            }

            const glm::vec2 position{ player->position() };
            const TilePos tile{ player_tile(position) };
            doc.players.push_back(PlayerJson{
                net_id,
                core::features::strip_colors(player->name()),
                position.x,
                position.y,
                tile.x,
                tile.y,
                player->is_local()
            });
        }

        for (const auto& object : world.get_object_map().get_objects()) {
            doc.objects.push_back(ObjectJson{ object.item_id, object.amount, object.pos.x, object.pos.y });
            doc.object_names.try_emplace(std::to_string(object.item_id), item_name(object.item_id));
        }
    }

    for (const TilePos tile : view.walk.path) {
        doc.path.push_back(PointJson{ tile.x, tile.y });
    }
    if (view.walk.target) {
        doc.has_target = true;
        doc.target = PointJson{ view.walk.target->x, view.walk.target->y };
    }

    return to_json(doc);
}

std::string result_json(
    const bool ok,
    const std::string_view action,
    const std::optional<TilePos> tile,
    const std::string_view message,
    const std::string_view reach,
    const std::string_view detail
)
{
    ResultJson doc{};
    doc.ok = ok;
    doc.action = action;
    doc.message = message;
    doc.reach = reach;
    doc.detail = detail;
    if (tile) {
        doc.has_tile = true;
        doc.tile = PointJson{ tile->x, tile->y };
    }
    return to_json(doc);
}
}
