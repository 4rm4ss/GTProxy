#include <cstdint>

#include <gtest/gtest.h>

#include "packet/game/world.hpp"

using namespace packet;

namespace {
// What the game server sends for a dropped object: the amount as a float,
// the item as int_data, the position in pixels.
GamePayload dropped(const std::int32_t change, const std::int32_t object_id, const float amount, const std::int32_t item, const float x, const float y)
{
    GameUpdatePacket header{};
    header.type = PACKET_ITEM_CHANGE_OBJECT;
    header.object_change_type = change;
    header.item_net_id = object_id;
    header.float_var = amount;
    header.int_data = item;
    header.pos_x = x;
    header.pos_y = y;
    return GamePayload{ header, {} };
}
}

TEST(ItemChangeObjectTest, ReadsAmountItemAndPositionLikeTheGameSendsThem)
{
    game::ItemChangeObject pkt{};
    ASSERT_TRUE(pkt.read(dropped(-1, 0, 90.0f, 242, 134.0f, 262.0f)));
    EXPECT_EQ(pkt.object_change_type, -1);
    EXPECT_EQ(pkt.item_id, 242);
    EXPECT_EQ(pkt.amount, 90);
    EXPECT_FLOAT_EQ(pkt.pos_x, 134.0f);
    EXPECT_FLOAT_EQ(pkt.pos_y, 262.0f);
}

TEST(ItemChangeObjectTest, CollectedByAPlayerNamesTheObject)
{
    game::ItemChangeObject pkt{};
    ASSERT_TRUE(pkt.read(dropped(30, 7, 0.0f, 0, 0.0f, 0.0f)));
    EXPECT_EQ(pkt.object_change_type, 30);
    EXPECT_EQ(pkt.item_net_id, 7u);
    EXPECT_EQ(pkt.amount, 0);
}

TEST(ItemChangeObjectTest, WritesBackWhatItRead)
{
    game::ItemChangeObject pkt{};
    ASSERT_TRUE(pkt.read(dropped(-3, 4, 12.0f, 2, 32.0f, 64.0f)));
    const auto out{ pkt.write() };
    const auto* game{ std::get_if<GamePayload>(&out) };
    ASSERT_NE(game, nullptr);
    EXPECT_EQ(game->packet.object_change_type, -3);
    EXPECT_EQ(game->packet.item_net_id, 4);
    EXPECT_FLOAT_EQ(game->packet.float_var, 12.0f);
    EXPECT_EQ(game->packet.int_data, 2);
    EXPECT_FLOAT_EQ(game->packet.pos_x, 32.0f);
    EXPECT_FLOAT_EQ(game->packet.pos_y, 64.0f);
}
