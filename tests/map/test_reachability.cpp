#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "map/reachability.hpp"

using map::Body;
using map::Cell;
using map::Reach;
using map::TilePos;

namespace {
// '.' free, '#' block, 'D' door, 'O' door open to you, '^' lava, 'P' free tile where you stand
struct Scene {
    map::Grid grid;
    glm::vec2 position{ 0.0f };
};

Scene scene(const std::vector<std::string>& rows)
{
    Scene s{};
    std::vector<Cell> cells;
    for (int y{ 0 }; y < static_cast<int>(rows.size()); ++y) {
        for (int x{ 0 }; x < static_cast<int>(rows[y].size()); ++x) {
            switch (rows[y][x]) {
            case '#': cells.push_back(Cell::Solid); break;
            case 'D': cells.push_back(Cell::Door); break;
            case 'O': cells.push_back(Cell::OpenDoor); break;
            case '^': cells.push_back(Cell::Hazard); break;
            case 'P':
                s.position = map::tile_to_world({ x, y });
                cells.push_back(Cell::Open);
                break;
            default: cells.push_back(Cell::Open); break;
            }
        }
    }
    s.grid = map::Grid{ { static_cast<int>(rows.front().size()), static_cast<int>(rows.size()) }, std::move(cells) };
    return s;
}

map::ReachResult reach(const Scene& s, const TilePos target, const Body& body = {})
{
    return map::check_reach(s.grid, s.position, body, target);
}

// Screenshot 1: you are walled in.
const std::vector<std::string> CLOSED_BOX{
    "..........",
    "..#####...",
    "..#...#...",
    "..#.P.#...",
    "..#...#...",
    "..#####...",
    "..........",
};

// Screenshot 2: the same box with the block on the left of you removed.
const std::vector<std::string> OPEN_BOX{
    "..........",
    "..#####...",
    "..#...#...",
    "....P.#...",
    "..#...#...",
    "..#####...",
    "..........",
};
}

TEST(MapReachabilityTest, ClosedBoxEveryTileOutsideIsUnreachable)
{
    const auto s{ scene(CLOSED_BOX) };
    for (const TilePos target : { TilePos{ 0, 0 }, TilePos{ 0, 3 }, TilePos{ 9, 6 }, TilePos{ 8, 3 }, TilePos{ 4, 6 } }) {
        const auto result{ reach(s, target) };
        EXPECT_EQ(result.status, Reach::Unreachable) << target.x << ", " << target.y;
        EXPECT_EQ(map::headline(result.status), "Цель недоступна");
    }

    // Inside the box you can move.
    const auto inside{ reach(s, { 3, 2 }) };
    EXPECT_EQ(inside.status, Reach::Reachable);
    EXPECT_EQ(inside.steps, 2u);
    ASSERT_TRUE(inside.start);
    EXPECT_EQ(*inside.start, (TilePos{ 4, 3 }));
}

TEST(MapReachabilityTest, OpeningInTheWallMakesOutsideReachable)
{
    const auto s{ scene(OPEN_BOX) };
    const auto result{ reach(s, { 0, 3 }) };
    EXPECT_EQ(result.status, Reach::Reachable);
    EXPECT_EQ(map::headline(result.status), "Цель доступна");
    EXPECT_EQ(result.steps, 4u);

    // Around the wall through the opening: out to (1, 3) (3 steps), up to
    // row 0 (3), along it to x = 7 (6), down to row 3 (3), right to (8, 3) (1).
    const auto behind{ reach(s, { 8, 3 }) };
    EXPECT_EQ(behind.status, Reach::Reachable);
    EXPECT_EQ(behind.steps, 16u);
}

TEST(MapReachabilityTest, TargetInABlockIsRejected)
{
    for (const auto& rows : { CLOSED_BOX, OPEN_BOX }) {
        const auto s{ scene(rows) };
        const auto result{ reach(s, { 6, 3 }) };
        EXPECT_EQ(result.status, Reach::TargetBlocked);
        EXPECT_EQ(map::headline(result.status), "Цель недоступна");
        EXPECT_EQ(map::reason(result.status), "в клетке блок");
    }

    const auto lava{ scene({ "P.^" }) };
    EXPECT_EQ(reach(lava, { 2, 0 }).status, Reach::TargetBlocked);
}

TEST(MapReachabilityTest, OutsideTheMapIsRejected)
{
    const auto s{ scene(OPEN_BOX) };
    for (const TilePos target : { TilePos{ -1, 0 }, TilePos{ 10, 0 }, TilePos{ 0, 7 }, TilePos{ 3, -5 } }) {
        const auto result{ reach(s, target) };
        EXPECT_EQ(result.status, Reach::OutOfBounds);
        EXPECT_EQ(map::reason(result.status), "за пределами карты");
    }
}

TEST(MapReachabilityTest, AnotherClosedAreaIsUnreachable)
{
    const auto s{ scene({
        "P.........",
        "......###.",
        "......#.#.",
        "......###.",
    }) };
    EXPECT_EQ(reach(s, { 7, 2 }).status, Reach::Unreachable);
    EXPECT_EQ(reach(s, { 9, 3 }).status, Reach::Reachable);
}

TEST(MapReachabilityTest, NoSqueezingBetweenCorners)
{
    // The only way out is diagonal, between two blocks touching at a corner.
    const auto s{ scene({
        "......",
        ".###..",
        ".#P#..",
        ".##...",
        "......",
    }) };
    EXPECT_EQ(reach(s, { 3, 3 }).status, Reach::Unreachable);
    EXPECT_EQ(reach(s, { 5, 4 }).status, Reach::Unreachable);
}

TEST(MapReachabilityTest, NoDataIsNotABlockedWay)
{
    const auto s{ scene(OPEN_BOX) };
    EXPECT_EQ(map::check_reach(map::Grid{}, s.position, {}, { 0, 0 }).status, Reach::NoData);
    EXPECT_EQ(map::check_reach(s.grid, std::nullopt, {}, { 0, 0 }).status, Reach::NoData);
    // A position outside the world is stale.
    EXPECT_EQ(map::check_reach(s.grid, glm::vec2{ -500.0f, 0.0f }, {}, { 0, 0 }).status, Reach::NoData);
    EXPECT_EQ(map::headline(Reach::NoData), "Нет данных");
}

TEST(MapReachabilityTest, DoorsOnlyAsTarget)
{
    const auto s{ scene({
        "#####",
        "P.D..",
        "#####",
    }) };
    EXPECT_EQ(reach(s, { 2, 1 }).status, Reach::Reachable);
    EXPECT_EQ(reach(s, { 4, 1 }).status, Reach::Unreachable);

    // You spawn on the main door: the search still starts there.
    const auto spawn{ scene({
        "#####",
        "D....",
        "#####",
    }) };
    EXPECT_EQ(map::check_reach(spawn.grid, map::tile_to_world({ 0, 1 }), {}, { 4, 1 }).status, Reach::Reachable);
}

TEST(MapReachabilityTest, DoorsOpenToYouLeadFurther)
{
    const auto s{ scene({
        "#####",
        "P.O..",
        "#####",
    }) };
    const auto r{ reach(s, { 4, 1 }) };
    EXPECT_EQ(r.status, Reach::Reachable);
    EXPECT_EQ(r.steps, 4u);
}

TEST(MapReachabilityTest, BodySizeDecidesWhereYouFit)
{
    // The normal 20x30 avatar covers one tile.
    const auto one{ map::covered_tiles({ 3, 4 }, Body{}) };
    EXPECT_EQ(one.first, (TilePos{ 3, 4 }));
    EXPECT_EQ(one.last, (TilePos{ 3, 4 }));

    // A 40x40 body covers 2x2 tiles.
    const Body big{ { 0, 0 }, { 40, 40 } };
    const auto two{ map::covered_tiles({ 3, 4 }, big) };
    EXPECT_EQ(two.first, (TilePos{ 3, 4 }));
    EXPECT_EQ(two.last, (TilePos{ 4, 5 }));

    // A one tile high gap lets the normal avatar through, not the big one.
    const auto s{ scene({
        "P...#....",
        ".........",
        "....#....",
    }) };
    EXPECT_EQ(reach(s, { 7, 0 }).status, Reach::Reachable);
    EXPECT_EQ(reach(s, { 7, 0 }, big).status, Reach::Unreachable);

    // The big body does not fit in the last column at all.
    EXPECT_EQ(reach(s, { 8, 0 }, big).status, Reach::TargetBlocked);
}

TEST(MapReachabilityTest, BodyFromColrect)
{
    const auto normal{ Body::from_colrect({ 0, 0, 20, 30 }) };
    EXPECT_EQ(normal.size, (glm::ivec2{ 20, 30 }));
    const auto missing{ Body::from_colrect({ 0, 0, 0, 0 }) };
    EXPECT_EQ(missing.size, (glm::ivec2{ 20, 30 }));
    const auto shifted{ Body::from_colrect({ 4, 2, 24, 28 }) };
    EXPECT_EQ(shifted.offset, (glm::ivec2{ 4, 2 }));

    // Your tile is the one under the centre of your body.
    EXPECT_EQ(map::body_tile({ 2745.0f, 736.0f }, normal), (TilePos{ 86, 23 }));
    EXPECT_EQ(map::body_tile({ 2720.0f, 736.0f }, normal), (TilePos{ 85, 23 }));
}
