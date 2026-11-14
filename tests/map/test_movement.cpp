#include <cmath>

#include <gtest/gtest.h>

#include "map/movement.hpp"

using map::Walker;

namespace {
constexpr float EPS{ 1e-3f };

void expect_near(const glm::vec2 a, const glm::vec2 b)
{
    EXPECT_NEAR(a.x, b.x, EPS);
    EXPECT_NEAR(a.y, b.y, EPS);
}
}

TEST(MapMovementTest, PathToPointsUsesTileCorners)
{
    const auto points{ map::path_to_points({ { 1, 2 }, { 2, 2 } }) };
    ASSERT_EQ(points.size(), 2u);
    expect_near(points[0], { 32.0f, 64.0f });
    expect_near(points[1], { 64.0f, 64.0f });
}

TEST(MapMovementTest, WalkerMovesAtConstantSpeed)
{
    Walker walker{ { { 0.0f, 0.0f }, { 64.0f, 0.0f } }, 32.0f };
    ASSERT_FALSE(walker.finished());

    auto step{ walker.advance(0.5f) };
    expect_near(step.position, { 16.0f, 0.0f });
    expect_near(step.velocity, { 32.0f, 0.0f });

    step = walker.advance(1.0f);
    expect_near(step.position, { 48.0f, 0.0f });
    EXPECT_FALSE(walker.finished());

    step = walker.advance(1.0f);
    expect_near(step.position, { 64.0f, 0.0f });
    expect_near(step.velocity, { 0.0f, 0.0f });
    EXPECT_TRUE(walker.finished());
}

TEST(MapMovementTest, WalkerTurnsCornersWithoutLosingDistance)
{
    // Right 32, then down 32; 48 px of travel ends 16 px down the second leg.
    Walker walker{ { { 0.0f, 0.0f }, { 32.0f, 0.0f }, { 32.0f, 32.0f } }, 48.0f };
    const auto step{ walker.advance(1.0f) };
    expect_near(step.position, { 32.0f, 16.0f });
    expect_near(step.velocity, { 0.0f, 48.0f });
    EXPECT_EQ(walker.next_point(), 2u);
}

TEST(MapMovementTest, WalkerNeverOvershootsTheEnd)
{
    Walker walker{ { { 0.0f, 0.0f }, { 32.0f, 32.0f } }, 1000.0f };
    const auto step{ walker.advance(10.0f) };
    expect_near(step.position, { 32.0f, 32.0f });
    EXPECT_TRUE(walker.finished());
}

TEST(MapMovementTest, InvalidWalkerIsFinished)
{
    EXPECT_TRUE((Walker{ {}, 100.0f }).finished());
    EXPECT_TRUE((Walker{ { { 0.0f, 0.0f }, { 32.0f, 0.0f } }, 0.0f }).finished());
    EXPECT_TRUE((Walker{ { { 0.0f, 0.0f } }, 100.0f }).finished());
}

TEST(MapMovementTest, StatePacketKeepsModelAndSetsMovement)
{
    packet::GameUpdatePacket model{};
    model.type = packet::PACKET_TILE_CHANGE_REQUEST;
    model.float_var = 1.5f;
    model.int_data = 18;
    model.int_x = 10;
    model.int_y = 11;
    model.data_size = 99;
    model.flags.value = static_cast<packet::PacketFlag>(
        packet::PACKET_FLAG_UNK | packet::PACKET_FLAG_ON_JUMP | packet::PACKET_FLAG_ON_PUNCHED
    );

    map::MoveState state{};
    state.position = { 100.0f, 200.0f };
    state.velocity = { -250.0f, 0.0f };
    state.facing_left = true;
    state.on_ground = true;

    const auto pkt{ map::make_state_packet(model, 7, state) };
    EXPECT_EQ(pkt.type, packet::PACKET_STATE);
    EXPECT_EQ(pkt.net_id, 7u);
    EXPECT_FLOAT_EQ(pkt.pos_x, 100.0f);
    EXPECT_FLOAT_EQ(pkt.pos_y, 200.0f);
    EXPECT_FLOAT_EQ(pkt.pos_x2, -250.0f);
    EXPECT_FLOAT_EQ(pkt.pos_y2, 0.0f);
    EXPECT_EQ(pkt.int_x, -1);
    EXPECT_EQ(pkt.int_y, -1);
    EXPECT_EQ(pkt.data_size, 0u);

    // Unknown fields come from the game's own packet.
    EXPECT_FLOAT_EQ(pkt.float_var, 1.5f);
    EXPECT_EQ(pkt.int_data, 18);

    const auto flags{ static_cast<std::uint32_t>(pkt.flags.value) };
    EXPECT_TRUE(flags & packet::PACKET_FLAG_UNK);
    EXPECT_TRUE(flags & packet::PACKET_FLAG_ROTATE_LEFT);
    EXPECT_TRUE(flags & packet::PACKET_FLAG_ON_SOLID);
    EXPECT_FALSE(flags & packet::PACKET_FLAG_ON_JUMP);
    EXPECT_FALSE(flags & packet::PACKET_FLAG_ON_PUNCHED);
}

TEST(MapMovementTest, StatePacketFacingRightInTheAir)
{
    packet::GameUpdatePacket model{};
    model.flags.value = static_cast<packet::PacketFlag>(packet::PACKET_FLAG_ROTATE_LEFT | packet::PACKET_FLAG_ON_SOLID);

    map::MoveState state{};
    state.position = { 1.0f, 2.0f };
    const auto pkt{ map::make_state_packet(model, 3, state) };
    const auto flags{ static_cast<std::uint32_t>(pkt.flags.value) };
    EXPECT_FALSE(flags & packet::PACKET_FLAG_ROTATE_LEFT);
    EXPECT_FALSE(flags & packet::PACKET_FLAG_ON_SOLID);
}
