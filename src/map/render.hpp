#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "coords.hpp"
#include "grid.hpp"

namespace world {
class World;
}

// A PNG of the map, drawn from the same state the map window shows: blocks
// in the colours of their seeds, backgrounds darker, dangerous tiles red,
// doors framed, dropped items, players, the modules' markers, routes and
// areas, and labels. Text is ASCII (other letters become '?').
namespace map {
struct RenderMarker {
    TilePos tile;
    std::string label;
    std::string color;      // "#rrggbb"
};

struct RenderRoute {
    std::vector<TilePos> tiles;
    std::string color;
};

struct RenderArea {
    TilePos from;
    TilePos to;
    std::string label;
    std::string color;
};

struct RenderInput {
    std::string title;      // "WORLD - 2026-11-15 16:20"
    std::vector<RenderMarker> markers;
    std::vector<RenderRoute> routes;
    std::vector<RenderArea> areas;
    int tile_pixels{ 16 };
};

// PNG file bytes; empty when there is no world.
[[nodiscard]] std::vector<std::uint8_t> render_png(const world::World& world, const Grid& grid, const RenderInput& input);

// The raw image, for tests: width, height and RGB rows.
struct Image {
    int width{ 0 };
    int height{ 0 };
    std::vector<std::uint8_t> rgb;

    [[nodiscard]] std::uint32_t at(int x, int y) const;
};
[[nodiscard]] Image render_image(const world::World& world, const Grid& grid, const RenderInput& input);
[[nodiscard]] std::vector<std::uint8_t> encode_png(const Image& image);
}
