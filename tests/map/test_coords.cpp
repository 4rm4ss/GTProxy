#include <limits>

#include <gtest/gtest.h>

#include "map/coords.hpp"

using map::TilePos;
using map::Viewport;

namespace {
Viewport view(const glm::vec2 origin, const float zoom, const glm::vec2 size = { 800.0f, 600.0f })
{
    Viewport v{};
    v.origin = origin;
    v.zoom = zoom;
    v.size = size;
    return v;
}
}

TEST(MapCoordsTest, WorldToTileFloorsIncludingNegatives)
{
    EXPECT_EQ(map::world_to_tile({ 0.0f, 0.0f }), (TilePos{ 0, 0 }));
    EXPECT_EQ(map::world_to_tile({ 31.9f, 63.9f }), (TilePos{ 0, 1 }));
    EXPECT_EQ(map::world_to_tile({ 32.0f, 64.0f }), (TilePos{ 1, 2 }));
    EXPECT_EQ(map::world_to_tile({ -0.5f, -32.0f }), (TilePos{ -1, -1 }));
}

TEST(MapCoordsTest, TileToWorldIsTheTopLeftCorner)
{
    // OnSetPos 2720, 736 from a real session is tile 85, 23.
    EXPECT_EQ(map::tile_to_world({ 85, 23 }), (glm::vec2{ 2720.0f, 736.0f }));
    EXPECT_EQ(map::world_to_tile(map::tile_to_world({ 85, 23 })), (TilePos{ 85, 23 }));
    EXPECT_EQ(map::tile_center({ 1, 1 }), (glm::vec2{ 48.0f, 48.0f }));
}

TEST(MapCoordsTest, PlayerTileUsesTheCentreOfTheAvatar)
{
    EXPECT_EQ(map::player_tile({ 2720.0f, 736.0f }), (TilePos{ 85, 23 }));
    // 20 px wide avatar at x = 2745 is mostly in the next tile.
    EXPECT_EQ(map::player_tile({ 2745.0f, 736.0f }), (TilePos{ 86, 23 }));
    EXPECT_EQ(map::player_tile({ 2730.0f, 736.0f }), (TilePos{ 85, 23 }));
}

TEST(MapCoordsTest, BoundsAndIndex)
{
    const glm::ivec2 size{ 100, 60 };
    EXPECT_TRUE(map::in_bounds({ 0, 0 }, size));
    EXPECT_TRUE(map::in_bounds({ 99, 59 }, size));
    EXPECT_FALSE(map::in_bounds({ 100, 0 }, size));
    EXPECT_FALSE(map::in_bounds({ 0, 60 }, size));
    EXPECT_FALSE(map::in_bounds({ -1, 5 }, size));
    EXPECT_EQ(map::tile_index({ 3, 2 }, 100), 203u);
}

TEST(MapCoordsTest, ScreenAndWorldRoundTrip)
{
    const auto v{ view({ 100.0f, 50.0f }, 2.0f) };
    const glm::vec2 world{ v.screen_to_world({ 40.0f, 30.0f }) };
    EXPECT_EQ(world, (glm::vec2{ 120.0f, 65.0f }));
    EXPECT_EQ(v.world_to_screen(world), (glm::vec2{ 40.0f, 30.0f }));
}

TEST(MapCoordsTest, CursorToTileAtZoomOne)
{
    const auto v{ view({ 0.0f, 0.0f }, 1.0f) };
    const glm::ivec2 size{ 100, 60 };
    EXPECT_EQ(map::screen_to_tile(v, { 0.0f, 0.0f }, size), (TilePos{ 0, 0 }));
    EXPECT_EQ(map::screen_to_tile(v, { 31.0f, 31.0f }, size), (TilePos{ 0, 0 }));
    EXPECT_EQ(map::screen_to_tile(v, { 32.0f, 70.0f }, size), (TilePos{ 1, 2 }));
}

TEST(MapCoordsTest, CursorToTileWithPanAndZoom)
{
    // Scrolled so tile (10, 5) starts at the canvas corner, 16 screen px per tile.
    const auto v{ view({ 320.0f, 160.0f }, 0.5f) };
    const glm::ivec2 size{ 100, 60 };
    EXPECT_EQ(map::screen_to_tile(v, { 0.0f, 0.0f }, size), (TilePos{ 10, 5 }));
    EXPECT_EQ(map::screen_to_tile(v, { 15.9f, 15.9f }, size), (TilePos{ 10, 5 }));
    EXPECT_EQ(map::screen_to_tile(v, { 16.0f, 48.0f }, size), (TilePos{ 11, 8 }));

    // Zoomed in: 128 screen px per tile.
    const auto close{ view({ 0.0f, 0.0f }, 4.0f) };
    EXPECT_EQ(map::screen_to_tile(close, { 127.0f, 129.0f }, size), (TilePos{ 0, 1 }));
}

TEST(MapCoordsTest, CursorOutsideWorldOrCanvasGivesNothing)
{
    const glm::ivec2 size{ 10, 10 };
    // The world ends at 320 px; the canvas is wider.
    const auto v{ view({ 0.0f, 0.0f }, 1.0f) };
    EXPECT_FALSE(map::screen_to_tile(v, { 320.0f, 10.0f }, size));
    EXPECT_FALSE(map::screen_to_tile(v, { 10.0f, 330.0f }, size));

    // Scrolled past the left edge of the world.
    const auto left{ view({ -64.0f, 0.0f }, 1.0f) };
    EXPECT_FALSE(map::screen_to_tile(left, { 10.0f, 10.0f }, size));
    EXPECT_EQ(map::screen_to_tile(left, { 64.0f, 10.0f }, size), (TilePos{ 0, 0 }));

    // Outside the canvas itself.
    EXPECT_FALSE(map::screen_to_tile(v, { -1.0f, 10.0f }, size));
    EXPECT_FALSE(map::screen_to_tile(v, { 800.0f, 10.0f }, size));
}

TEST(MapCoordsTest, InvalidViewGivesNothing)
{
    const glm::ivec2 size{ 10, 10 };
    EXPECT_FALSE(map::screen_to_tile(view({ 0.0f, 0.0f }, 0.0f), { 1.0f, 1.0f }, size));
    EXPECT_FALSE(map::screen_to_tile(view({ 0.0f, 0.0f }, 100.0f), { 1.0f, 1.0f }, size));
    EXPECT_FALSE(map::screen_to_tile(view({ 0.0f, 0.0f }, 1.0f, { 0.0f, 600.0f }), { 1.0f, 1.0f }, size));
    const float nan{ std::numeric_limits<float>::quiet_NaN() };
    EXPECT_FALSE(map::screen_to_tile(view({ nan, 0.0f }, 1.0f), { 1.0f, 1.0f }, size));
    EXPECT_FALSE(map::screen_to_tile(view({ 0.0f, 0.0f }, 1.0f), { nan, 1.0f }, size));
}
