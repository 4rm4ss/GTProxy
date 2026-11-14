#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "item/item_database.hpp"
#include "utils/byte_stream.hpp"
#include "world/world.hpp"

namespace {
// A world 3x2 without items.dat: every tile has a background, (1, 0) a block.
std::vector<std::byte> small_world()
{
    utils::ByteStream<> bs{};
    bs.write<std::uint16_t>(0x18);
    bs.write<std::uint32_t>(0);
    bs.write(std::string{ "UPDATES" });
    bs.write<std::int32_t>(3);
    bs.write<std::int32_t>(2);
    bs.write<std::uint32_t>(6);
    for (int i = 0; i < 5; ++i) {
        bs.write<std::uint8_t>(0);
    }
    for (int i = 0; i < 6; ++i) {
        bs.write<std::uint16_t>(i == 1 ? 2 : 0);
        bs.write<std::uint16_t>(14);
        bs.write<std::uint16_t>(0);
        bs.write<std::uint16_t>(0);
    }
    for (int i = 0; i < 12; ++i) {
        bs.write<std::uint8_t>(0);
    }
    bs.write<std::uint32_t>(0);
    bs.write<std::uint32_t>(0);
    const auto* begin{ bs.get_raw_ptr() };
    return { begin, begin + bs.get_size() };
}

std::vector<std::byte> plain_tile(const std::uint16_t fg, const std::uint16_t bg)
{
    utils::ByteStream<> bs{};
    bs.write<std::uint16_t>(fg);
    bs.write<std::uint16_t>(bg);
    bs.write<std::uint16_t>(0);
    bs.write<std::uint16_t>(0);
    const auto* begin{ bs.get_raw_ptr() };
    return { begin, begin + bs.get_size() };
}
}

class WorldUpdatesTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        item::ItemDatabase::instance().clear();
        const auto data{ small_world() };
        world_.clear();
        ASSERT_TRUE(world_.serialize(data.data(), data.size()));
    }

    world::World world_{};
};

TEST_F(WorldUpdatesTest, PunchBreaksTheBlockThenTheBackground)
{
    ASSERT_EQ(world_.get_tile(1, 0)->foreground, 2);

    EXPECT_TRUE(world_.change_tile(1, 0, 18));
    EXPECT_EQ(world_.get_tile(1, 0)->foreground, 0);
    EXPECT_EQ(world_.get_tile(1, 0)->background, 14);

    EXPECT_TRUE(world_.change_tile(1, 0, 18));
    EXPECT_EQ(world_.get_tile(1, 0)->background, 0);

    EXPECT_FALSE(world_.change_tile(1, 0, 18));
}

TEST_F(WorldUpdatesTest, PlacingNeedsItemsDat)
{
    // Without items.dat the proxy cannot tell a block from a background.
    EXPECT_FALSE(world_.change_tile(0, 0, 2));
    EXPECT_EQ(world_.get_tile(0, 0)->foreground, 0);
}

TEST_F(WorldUpdatesTest, OutsideTheWorldIsIgnored)
{
    EXPECT_FALSE(world_.change_tile(3, 0, 18));
    EXPECT_FALSE(world_.change_tile(-1, 0, 18));
    EXPECT_EQ(world_.get_tile(0, 2), nullptr);

    const auto tile{ plain_tile(2, 0) };
    EXPECT_FALSE(world_.update_tile(0, 5, tile.data(), tile.size()));
}

TEST_F(WorldUpdatesTest, TileUpdateReplacesTheTile)
{
    const auto tile{ plain_tile(10, 0) };
    EXPECT_TRUE(world_.update_tile(2, 1, tile.data(), tile.size()));
    EXPECT_EQ(world_.get_tile(2, 1)->foreground, 10);
    EXPECT_EQ(world_.get_tile(2, 1)->background, 0);
}

TEST_F(WorldUpdatesTest, TruncatedTileUpdateKeepsTheOldTile)
{
    const auto tile{ plain_tile(10, 0) };
    EXPECT_FALSE(world_.update_tile(1, 0, tile.data(), 3));
    EXPECT_EQ(world_.get_tile(1, 0)->foreground, 2);
}
