#pragma once
#include <cmath>
#include <cstddef>
#include <optional>

#include <glm/glm.hpp>

// Coordinate systems of the map window and the click handler:
//
//   tile    cell of the world grid, (0, 0) = top-left; what /pos shows
//   world   Growtopia pixels, 32 per tile. Player and dropped item positions
//           are sent in these units (OnSpawn posXY, state packets, OnSetPos)
//   screen  CSS pixels inside the map canvas, (0, 0) = its top-left corner
//
// All three grow to the right and downwards.
namespace map {
inline constexpr int TILE_SIZE{ 32 };

// Collision box of an avatar ("colrect|0|0|20|30" in OnSpawn), measured from
// the player's position.
inline constexpr glm::vec2 PLAYER_SIZE{ 20.0f, 30.0f };

struct TilePos {
    int x{ 0 };
    int y{ 0 };

    friend bool operator==(const TilePos&, const TilePos&) = default;
};

[[nodiscard]] inline TilePos world_to_tile(const glm::vec2 world)
{
    return {
        static_cast<int>(std::floor(world.x / static_cast<float>(TILE_SIZE))),
        static_cast<int>(std::floor(world.y / static_cast<float>(TILE_SIZE)))
    };
}

// Top-left corner of a tile. A player standing in a tile has exactly this
// position (spawn points and doors put you there).
[[nodiscard]] inline glm::vec2 tile_to_world(const TilePos tile)
{
    return { static_cast<float>(tile.x * TILE_SIZE), static_cast<float>(tile.y * TILE_SIZE) };
}

[[nodiscard]] inline glm::vec2 tile_center(const TilePos tile)
{
    return tile_to_world(tile) + glm::vec2{ TILE_SIZE / 2.0f };
}

// The tile an avatar is in: the one under the centre of its collision box.
[[nodiscard]] inline TilePos player_tile(const glm::vec2 position)
{
    return world_to_tile(position + PLAYER_SIZE * 0.5f);
}

[[nodiscard]] inline bool in_bounds(const TilePos tile, const glm::ivec2 size)
{
    return tile.x >= 0 && tile.y >= 0 && tile.x < size.x && tile.y < size.y;
}

[[nodiscard]] inline std::size_t tile_index(const TilePos tile, const int width)
{
    return static_cast<std::size_t>(tile.y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(tile.x);
}

// What part of the world the map canvas shows.
struct Viewport {
    static constexpr float MIN_ZOOM{ 0.02f };
    static constexpr float MAX_ZOOM{ 16.0f };

    glm::vec2 origin{ 0.0f };   // world point at the canvas top-left corner
    float zoom{ 1.0f };         // screen pixels per world pixel
    glm::vec2 size{ 0.0f };     // canvas size in screen pixels

    [[nodiscard]] bool valid() const
    {
        return std::isfinite(origin.x) && std::isfinite(origin.y)
            && std::isfinite(zoom) && zoom >= MIN_ZOOM && zoom <= MAX_ZOOM
            && std::isfinite(size.x) && std::isfinite(size.y)
            && size.x > 0.0f && size.y > 0.0f;
    }

    [[nodiscard]] glm::vec2 screen_to_world(const glm::vec2 screen) const { return origin + screen / zoom; }
    [[nodiscard]] glm::vec2 world_to_screen(const glm::vec2 world) const { return (world - origin) * zoom; }
};

// Cursor -> tile: the tile under a point of the canvas. Nothing when the view
// is invalid, the point is outside the canvas or there is no world there.
[[nodiscard]] inline std::optional<TilePos> screen_to_tile(
    const Viewport& view,
    const glm::vec2 screen,
    const glm::ivec2 world_size
)
{
    if (!view.valid() || !std::isfinite(screen.x) || !std::isfinite(screen.y)) {
        return std::nullopt;
    }

    if (screen.x < 0.0f || screen.y < 0.0f || screen.x >= view.size.x || screen.y >= view.size.y) {
        return std::nullopt;
    }

    const TilePos tile{ world_to_tile(view.screen_to_world(screen)) };
    if (!in_bounds(tile, world_size)) {
        return std::nullopt;
    }

    return tile;
}
}
