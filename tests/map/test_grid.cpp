#include <gtest/gtest.h>

#include "item/item_database.hpp"
#include "map/grid.hpp"
#include "world/tile_map.hpp"

using map::Cell;
using world::TileFlag;

namespace {
item::ItemInfo make_item(const item::ItemType type, const item::CollisionType collision)
{
    item::ItemInfo info{};
    info.item_id = 100;
    info.item_type = type;
    info.collision_type = collision;
    return info;
}
}

TEST(MapGridTest, EmptyForegroundIsOpen)
{
    EXPECT_EQ(map::classify(0, TileFlag::None, nullptr), Cell::Open);
}

TEST(MapGridTest, UnknownItemIsSolid)
{
    EXPECT_EQ(map::classify(9999, TileFlag::None, nullptr), Cell::Solid);
}

TEST(MapGridTest, CollisionTypes)
{
    using item::CollisionType;
    using item::ItemType;
    const auto cls = [](const CollisionType collision, const TileFlag flags = TileFlag::None) {
        const auto info{ make_item(ItemType::Normal, collision) };
        return map::classify(2, flags, &info);
    };

    EXPECT_EQ(cls(CollisionType::None), Cell::Open);
    EXPECT_EQ(cls(CollisionType::Full), Cell::Solid);
    EXPECT_EQ(cls(CollisionType::JumpThrough), Cell::Platform);
    EXPECT_EQ(cls(CollisionType::JumpDown), Cell::Platform);
    EXPECT_EQ(cls(CollisionType::OneWay), Cell::Solid);

    // Entrances: only public ones are walked through.
    EXPECT_EQ(cls(CollisionType::Gateway), Cell::Solid);
    EXPECT_EQ(cls(CollisionType::Gateway, TileFlag::Public), Cell::Open);
    EXPECT_EQ(cls(CollisionType::VipDoor, TileFlag::Public), Cell::Open);

    // Switchable blocks.
    EXPECT_EQ(cls(CollisionType::CollideIfOff), Cell::Solid);
    EXPECT_EQ(cls(CollisionType::CollideIfOff, TileFlag::Open), Cell::Open);
    EXPECT_EQ(cls(CollisionType::CollideIfOn), Cell::Open);
    EXPECT_EQ(cls(CollisionType::CollideIfOn, TileFlag::Open), Cell::Solid);
}

TEST(MapGridTest, HazardsAndDoorsWhateverTheirCollision)
{
    const auto lava{ make_item(item::ItemType::Lava, item::CollisionType::Full) };
    const auto spikes{ make_item(item::ItemType::Deadly, item::CollisionType::None) };
    const auto door{ make_item(item::ItemType::Door, item::CollisionType::None) };
    const auto portal{ make_item(item::ItemType::Portal, item::CollisionType::None) };
    EXPECT_EQ(map::classify(4, TileFlag::None, &lava), Cell::Hazard);
    EXPECT_EQ(map::classify(4, TileFlag::None, &spikes), Cell::Hazard);
    EXPECT_EQ(map::classify(6, TileFlag::None, &door), Cell::Door);
    EXPECT_EQ(map::classify(6, TileFlag::None, &portal), Cell::Door);
}

TEST(MapGridTest, PassableAndStandable)
{
    EXPECT_TRUE(map::is_passable(Cell::Open));
    EXPECT_TRUE(map::is_passable(Cell::Platform));
    EXPECT_FALSE(map::is_passable(Cell::Solid));
    EXPECT_FALSE(map::is_passable(Cell::Hazard));
    EXPECT_FALSE(map::is_passable(Cell::Door));

    EXPECT_TRUE(map::can_stand_on(Cell::Solid));
    EXPECT_TRUE(map::can_stand_on(Cell::Platform));
    EXPECT_FALSE(map::can_stand_on(Cell::Open));
    EXPECT_FALSE(map::can_stand_on(Cell::Hazard));
}

TEST(MapGridTest, GridRejectsMismatchedSizes)
{
    EXPECT_TRUE(map::Grid({ 2, 2 }, std::vector<Cell>(3, Cell::Open)).empty());
    EXPECT_TRUE(map::Grid({ 0, 2 }, {}).empty());

    const map::Grid grid{ { 2, 1 }, { Cell::Open, Cell::Solid } };
    ASSERT_FALSE(grid.empty());
    EXPECT_EQ(grid.at({ 0, 0 }), Cell::Open);
    EXPECT_EQ(grid.at({ 1, 0 }), Cell::Solid);
    // Outside the world counts as solid.
    EXPECT_EQ(grid.at({ -1, 0 }), Cell::Solid);
    EXPECT_EQ(grid.at({ 0, 1 }), Cell::Solid);
}

TEST(MapGridTest, FromTileMapWithoutItemsDat)
{
    item::ItemDatabase::instance().clear();

    WorldTileMap tile_map{};
    tile_map.get_size() = { 3, 1 };
    tile_map.get_tiles().resize(3);
    tile_map.get_tiles()[1].foreground = 2;     // a dirt block
    tile_map.get_tiles()[2].background = 14;    // only a background

    const auto grid{ map::Grid::from_tile_map(tile_map) };
    ASSERT_EQ(grid.size(), (glm::ivec2{ 3, 1 }));
    EXPECT_EQ(grid.at({ 0, 0 }), Cell::Open);
    EXPECT_EQ(grid.at({ 1, 0 }), Cell::Solid);
    EXPECT_EQ(grid.at({ 2, 0 }), Cell::Open);
}
