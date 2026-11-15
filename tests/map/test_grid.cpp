#include <filesystem>
#include <optional>

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

namespace {
// A 4x1 world: a door at 1, 0 and, if given, a lock at 3, 0.
WorldTileMap door_world(const std::uint16_t door_item, const TileFlag door_flags, const std::optional<world::tile_extra::Lock> lock, const std::uint16_t lock_item = 242)
{
    WorldTileMap tile_map{};
    tile_map.get_size() = { 4, 1 };
    tile_map.get_tiles().resize(4);
    auto& door{ tile_map.get_tiles()[1] };
    door.foreground = door_item;
    door.flag = door_flags;
    if (lock) {
        auto& tile{ tile_map.get_tiles()[3] };
        tile.foreground = lock_item;
        tile.extra.type_ = world::tile_extra::Type::Lock;
        tile.extra.data_ = *lock;
        if (lock_item == 202) {     // a Small Lock covers the door
            door.flag = door.flag | TileFlag::Locked;
            door.lock_parent_tile = 3;
        }
    }
    return tile_map;
}

// The bundled items.dat (resources/): what the game really calls doors.
bool load_items_dat()
{
    const std::filesystem::path path{ std::filesystem::path{ GTPROXY_RESOURCES_DIR } / "items.dat" };
    return std::filesystem::exists(path) && item::ItemDatabase::instance().load_from_file(path.string());
}
}

TEST(MapGridTest, DoorsOpenToYouAreWalkedThrough)
{
    if (!load_items_dat()) {
        GTEST_SKIP() << "resources/items.dat not found";
    }
    // 6 White Door: every world's main door (EXIT); 12 Door: a door you place.
    const auto& items{ item::ItemDatabase::instance() };
    ASSERT_NE(items.get_item(6), nullptr);
    ASSERT_NE(items.get_item(12), nullptr);
    EXPECT_EQ(items.get_item(6)->item_type, item::ItemType::Door) << items.get_item(6)->item_name;
    EXPECT_EQ(items.get_item(12)->item_type, item::ItemType::UserDoor) << items.get_item(12)->item_name;

    const world::tile_extra::Lock mine{ 0, 1005, {} };
    const world::tile_extra::Lock theirs{ 0, 2000, { 1005 } };
    const world::tile_extra::Lock stranger{ 0, 2000, { 3000 } };
    const map::Doors me{ map::DoorMode::Access, 1005 };
    const auto at_door = [](const WorldTileMap& tile_map, const map::Doors& doors) {
        return map::Grid::from_tile_map(tile_map, doors).at({ 1, 0 });
    };

    // No lock in the world: anyone walks through.
    EXPECT_EQ(at_door(door_world(12, TileFlag::None, std::nullopt), me), Cell::OpenDoor);
    for (std::uint32_t id{ 0 }; id < items.get_count(); ++id) {
        if (const auto* info{ items.get_item(id) }; info && info->item_type == item::ItemType::Portal) {
            EXPECT_EQ(at_door(door_world(static_cast<std::uint16_t>(id), TileFlag::None, std::nullopt), me), Cell::OpenDoor) << info->item_name;
            break;
        }
    }
    // A World Lock: the owner and those with access, or a public door.
    EXPECT_EQ(at_door(door_world(12, TileFlag::None, mine), me), Cell::OpenDoor);
    EXPECT_EQ(at_door(door_world(12, TileFlag::None, theirs), me), Cell::OpenDoor);
    EXPECT_EQ(at_door(door_world(12, TileFlag::None, stranger), me), Cell::Door);
    EXPECT_EQ(at_door(door_world(12, TileFlag::Public, stranger), me), Cell::OpenDoor);
    // Who you are is not known yet: a locked door stays closed.
    EXPECT_EQ(at_door(door_world(12, TileFlag::None, mine), map::Doors{ map::DoorMode::Access, 0 }), Cell::Door);
    // A Small Lock over the door decides, not the world.
    EXPECT_EQ(at_door(door_world(12, TileFlag::None, mine, 202), me), Cell::OpenDoor);
    EXPECT_EQ(at_door(door_world(12, TileFlag::None, stranger, 202), me), Cell::Door);
    // The main door (EXIT) is never walked through; "none" turns it all off.
    EXPECT_EQ(at_door(door_world(6, TileFlag::None, std::nullopt), me), Cell::Door);
    EXPECT_EQ(at_door(door_world(12, TileFlag::None, std::nullopt), map::Doors{ map::DoorMode::None, 1005 }), Cell::Door);

    EXPECT_TRUE(map::is_passable(Cell::OpenDoor));
    EXPECT_FALSE(map::can_stand_on(Cell::OpenDoor));
    item::ItemDatabase::instance().clear();
}

TEST(MapGridTest, EntrancesFollowTheLockToo)
{
    if (!load_items_dat()) {
        GTEST_SKIP() << "resources/items.dat not found";
    }
    // 224 House Entrance, 686 Jail Door: entrances (collision "gateway").
    const auto& items{ item::ItemDatabase::instance() };
    ASSERT_NE(items.get_item(224), nullptr);
    EXPECT_EQ(items.get_item(224)->collision_type, item::CollisionType::Gateway) << items.get_item(224)->item_name;

    const world::tile_extra::Lock mine{ 0, 1005, {} };
    const world::tile_extra::Lock stranger{ 0, 2000, { 3000 } };
    const map::Doors me{ map::DoorMode::Access, 1005 };
    const auto at = [](const WorldTileMap& tile_map, const map::Doors& doors) {
        return map::Grid::from_tile_map(tile_map, doors).at({ 1, 0 });
    };

    // Your own world: your House Entrance lets you in.
    EXPECT_EQ(at(door_world(224, TileFlag::None, mine), me), Cell::OpenDoor);
    EXPECT_EQ(at(door_world(686, TileFlag::None, mine), me), Cell::OpenDoor);
    // Someone else's: closed (you bump into it), unless public.
    EXPECT_EQ(at(door_world(224, TileFlag::None, stranger), me), Cell::Solid);
    EXPECT_EQ(at(door_world(224, TileFlag::Public, stranger), me), Cell::Open);
    // "all": you know you may pass; "none": entrances only when public.
    EXPECT_EQ(at(door_world(224, TileFlag::None, stranger), map::Doors{ map::DoorMode::All, 1005 }), Cell::OpenDoor);
    EXPECT_EQ(at(door_world(12, TileFlag::None, stranger), map::Doors{ map::DoorMode::All, 1005 }), Cell::OpenDoor);
    EXPECT_EQ(at(door_world(6, TileFlag::None, std::nullopt), map::Doors{ map::DoorMode::All, 1005 }), Cell::Door);
    EXPECT_EQ(at(door_world(224, TileFlag::None, mine), map::Doors{ map::DoorMode::None, 1005 }), Cell::Solid);

    // Why a door is closed, for the "no way" message.
    EXPECT_EQ(map::closed_because(door_world(224, TileFlag::None, stranger), { 1, 0 }, me), "it is locked by user 2000 (you are 1005)");
    EXPECT_EQ(map::closed_because(door_world(224, TileFlag::None, mine), { 1, 0 }, map::Doors{ map::DoorMode::Access, 0 }), "it is locked and your user ID is not known yet");
    EXPECT_EQ(map::closed_because(door_world(6, TileFlag::None, std::nullopt), { 1, 0 }, me), "it is the world's main door (EXIT)");
    EXPECT_EQ(map::closed_because(door_world(224, TileFlag::None, mine), { 1, 0 }, me), "");

    EXPECT_EQ(map::door_mode("all"), map::DoorMode::All);
    EXPECT_FALSE(map::door_mode("open"));
    item::ItemDatabase::instance().clear();
}
