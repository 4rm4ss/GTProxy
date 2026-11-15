#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "map/pathfinder.hpp"

using map::Cell;
using map::PathError;
using map::TilePos;

namespace {
// '.' open, '#' solid, '=' platform, '^' hazard, 'D' door, 'O' door open to you
map::Grid grid_from(const std::vector<std::string>& rows)
{
    const int height{ static_cast<int>(rows.size()) };
    const int width{ static_cast<int>(rows.front().size()) };
    std::vector<Cell> cells;
    for (const auto& row : rows) {
        for (const char c : row) {
            switch (c) {
            case '#': cells.push_back(Cell::Solid); break;
            case '=': cells.push_back(Cell::Platform); break;
            case '^': cells.push_back(Cell::Hazard); break;
            case 'D': cells.push_back(Cell::Door); break;
            case 'O': cells.push_back(Cell::OpenDoor); break;
            default: cells.push_back(Cell::Open); break;
            }
        }
    }
    return map::Grid{ { width, height }, std::move(cells) };
}

// Every step goes to a neighbouring tile and never through a blocked one.
void expect_valid(const map::Grid& grid, const map::Path& path, const TilePos start, const TilePos goal)
{
    ASSERT_FALSE(path.empty());
    EXPECT_EQ(path.front(), start);
    EXPECT_EQ(path.back(), goal);
    for (std::size_t i{ 1 }; i < path.size(); ++i) {
        const int dx{ std::abs(path[i].x - path[i - 1].x) };
        const int dy{ std::abs(path[i].y - path[i - 1].y) };
        EXPECT_LE(dx, 1);
        EXPECT_LE(dy, 1);
        EXPECT_GT(dx + dy, 0);
        if (i + 1 < path.size()) {
            EXPECT_TRUE(map::is_passable(grid.at(path[i]))) << "step " << i;
        }
        if (dx == 1 && dy == 1) {
            EXPECT_TRUE(map::is_passable(grid.at({ path[i].x, path[i - 1].y }))) << "cut corner at step " << i;
            EXPECT_TRUE(map::is_passable(grid.at({ path[i - 1].x, path[i].y }))) << "cut corner at step " << i;
        }
    }
}

map::PathOptions on_foot(const int jump)
{
    map::PathOptions options{};
    options.gravity = true;
    options.jump_height = jump;
    return options;
}
}

TEST(MapPathfinderTest, StraightLine)
{
    const auto grid{ grid_from({
        ".....",
    }) };
    const auto path{ map::find_path(grid, { 0, 0 }, { 4, 0 }) };
    ASSERT_TRUE(path);
    EXPECT_EQ(path->size(), 5u);
    expect_valid(grid, *path, { 0, 0 }, { 4, 0 });
}

TEST(MapPathfinderTest, DiagonalWhenFree)
{
    const auto grid{ grid_from({
        "....",
        "....",
        "....",
        "....",
    }) };
    const auto path{ map::find_path(grid, { 0, 0 }, { 3, 3 }) };
    ASSERT_TRUE(path);
    EXPECT_EQ(path->size(), 4u);
    expect_valid(grid, *path, { 0, 0 }, { 3, 3 });
}

TEST(MapPathfinderTest, GoesAroundWalls)
{
    const auto grid{ grid_from({
        ".....",
        ".###.",
        ".#...",
        ".#.#.",
        "...#.",
    }) };
    const auto path{ map::find_path(grid, { 2, 2 }, { 0, 0 }) };
    ASSERT_TRUE(path);
    expect_valid(grid, *path, { 2, 2 }, { 0, 0 });
}

TEST(MapPathfinderTest, NeverCutsCorners)
{
    const auto grid{ grid_from({
        ".#",
        "#.",
    }) };
    EXPECT_EQ(map::find_path(grid, { 0, 0 }, { 1, 1 }).error(), PathError::NoPath);

    const auto half{ grid_from({
        "..",
        "#.",
    }) };
    const auto path{ map::find_path(half, { 0, 0 }, { 1, 1 }) };
    ASSERT_TRUE(path);
    EXPECT_EQ(path->size(), 3u);
    expect_valid(half, *path, { 0, 0 }, { 1, 1 });
}

TEST(MapPathfinderTest, AvoidsHazardsAndPassesPlatforms)
{
    const auto grid{ grid_from({
        "..^..",
        "..=..",
        "..^..",
    }) };
    const auto path{ map::find_path(grid, { 0, 1 }, { 4, 1 }) };
    ASSERT_TRUE(path);
    expect_valid(grid, *path, { 0, 1 }, { 4, 1 });
    for (const auto tile : *path) {
        EXPECT_NE(grid.at(tile), Cell::Hazard);
    }
}

TEST(MapPathfinderTest, DoorsOnlyAsDestination)
{
    const auto grid{ grid_from({
        "#####",
        "..D..",
        "#####",
    }) };
    EXPECT_EQ(map::find_path(grid, { 0, 1 }, { 4, 1 }).error(), PathError::NoPath);

    const auto into_door{ map::find_path(grid, { 0, 1 }, { 2, 1 }) };
    ASSERT_TRUE(into_door);
    EXPECT_EQ(into_door->back(), (TilePos{ 2, 1 }));
}

TEST(MapPathfinderTest, ThroughADoorOpenToYou)
{
    const auto grid{ grid_from({
        "#####",
        "..O..",
        "#####",
    }) };
    const auto path{ map::find_path(grid, { 0, 1 }, { 4, 1 }) };
    ASSERT_TRUE(path);
    EXPECT_EQ(path->back(), (TilePos{ 4, 1 }));
    EXPECT_NE(std::ranges::find(*path, TilePos{ 2, 1 }), path->end());
}

TEST(MapPathfinderTest, Errors)
{
    const auto grid{ grid_from({
        "..#",
        "...",
    }) };
    EXPECT_EQ(map::find_path(map::Grid{}, { 0, 0 }, { 1, 0 }).error(), PathError::EmptyWorld);
    EXPECT_EQ(map::find_path(grid, { -1, 0 }, { 1, 0 }).error(), PathError::StartOutside);
    EXPECT_EQ(map::find_path(grid, { 0, 0 }, { 3, 0 }).error(), PathError::GoalOutside);
    EXPECT_EQ(map::find_path(grid, { 0, 0 }, { 2, 0 }).error(), PathError::GoalBlocked);

    const auto here{ map::find_path(grid, { 1, 1 }, { 1, 1 }) };
    ASSERT_TRUE(here);
    EXPECT_EQ(here->size(), 1u);
}

TEST(MapPathfinderTest, EnclosedAreaHasNoPath)
{
    const auto grid{ grid_from({
        "......",
        ".####.",
        ".#..#.",
        ".####.",
    }) };
    EXPECT_EQ(map::find_path(grid, { 0, 0 }, { 2, 2 }).error(), PathError::NoPath);
}

TEST(MapPathfinderTest, ClickOnABlockWalksNextToIt)
{
    const auto grid{ grid_from({
        "........",
        "........",
        "....#...",
        "########",
    }) };
    const auto dest{ map::find_destination(grid, { 0, 2 }, { 4, 2 }) };
    ASSERT_TRUE(dest);
    EXPECT_TRUE(dest->next_to_goal);
    // The free tile beside the block on your side.
    EXPECT_EQ(dest->tile, (TilePos{ 3, 2 }));
    expect_valid(grid, dest->path, { 0, 2 }, { 3, 2 });

    // An open tile is walked to directly.
    const auto open{ map::find_destination(grid, { 0, 2 }, { 6, 1 }) };
    ASSERT_TRUE(open);
    EXPECT_FALSE(open->next_to_goal);
    EXPECT_EQ(open->tile, (TilePos{ 6, 1 }));

    // A block buried in other blocks.
    const auto buried{ grid_from({
        "###",
        "###",
        "###",
    }) };
    EXPECT_EQ(map::find_destination(buried, { 0, 0 }, { 1, 1 }).error(), PathError::GoalBlocked);
}

TEST(MapPathfinderTest, GravityLimitsHowHighYouJump)
{
    // Ground at the bottom, a 3 tiles high wall with a ledge on top.
    const auto grid{ grid_from({
        "..........",
        "..........",
        ".....#####",
        ".....#....",
        ".....#....",
        "##########",
    }) };
    const TilePos start{ 1, 4 };
    const TilePos ledge{ 7, 1 };

    const auto high{ map::find_path(grid, start, ledge, on_foot(3)) };
    ASSERT_TRUE(high);
    expect_valid(grid, *high, start, ledge);

    EXPECT_EQ(map::find_path(grid, start, ledge, on_foot(2)).error(), PathError::NoPath);

    // Ignoring gravity anything open is reachable.
    EXPECT_TRUE(map::find_path(grid, start, ledge));
}

TEST(MapPathfinderTest, GravityCannotFloatOverGaps)
{
    // A gap 6 tiles wide over a pit: too far to jump with height 1.
    const auto grid{ grid_from({
        "..........",
        "..........",
        "##......##",
        "##......##",
        "##......##",
    }) };
    const TilePos start{ 0, 1 };
    const TilePos other_side{ 9, 1 };

    EXPECT_EQ(map::find_path(grid, start, other_side, on_foot(1)).error(), PathError::NoPath);
    EXPECT_TRUE(map::find_path(grid, start, other_side));

    // Falling into the pit is fine.
    const auto down{ map::find_path(grid, start, { 4, 4 }, on_foot(1)) };
    ASSERT_TRUE(down);
    expect_valid(grid, *down, start, { 4, 4 });
}

TEST(MapPathfinderTest, GravityPlatformsCanBeStoodOn)
{
    // Jump onto a platform, then on to the next one.
    const auto grid{ grid_from({
        "........",
        "....===.",
        "........",
        "........",
        "===.....",
        "########",
    }) };
    const auto path{ map::find_path(grid, { 0, 3 }, { 6, 0 }, on_foot(3)) };
    ASSERT_TRUE(path);
    expect_valid(grid, *path, { 0, 3 }, { 6, 0 });
}
