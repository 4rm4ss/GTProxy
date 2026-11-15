#include <limits>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "core/handlers/player_state.hpp"

using namespace core::player_state;

namespace {
packet::GameUpdatePacket header_of(const std::vector<std::byte>& data)
{
    packet::GameUpdatePacket header{};
    std::memcpy(&header, data.data() + sizeof(std::uint32_t), sizeof(header));
    return header;
}

std::vector<std::byte> state_report(const std::uint32_t flags, const float x, const float y)
{
    packet::GameUpdatePacket header{};
    header.type = packet::PACKET_STATE;
    header.net_id = 5;
    header.flags.value = static_cast<packet::PacketFlag>(flags);
    header.pos_x = x;
    header.pos_y = y;
    std::vector<std::byte> data(sizeof(std::uint32_t) + sizeof(header));
    const std::uint32_t type{ packet::NET_MESSAGE_GAME_PACKET };
    std::memcpy(data.data(), &type, sizeof(type));
    std::memcpy(data.data() + sizeof(type), &header, sizeof(header));
    return data;
}
}

TEST(PlayerStateTest, UsualStateHasTheValuesServersSend)
{
    const auto header{ header_of(usual_state(7)) };
    EXPECT_EQ(header.type, packet::PACKET_SET_CHARACTER_STATE);
    EXPECT_EQ(header.net_id, 7u);
    EXPECT_FLOAT_EQ(header.pos_x2, 300.0f);
    EXPECT_FLOAT_EQ(header.pos_y2, 600.0f);
    EXPECT_FLOAT_EQ(header.float_var, 125.0f);
    EXPECT_EQ(header.pad[1], 0x80);
    EXPECT_EQ(header.pad[2], 0x80);
}

TEST(PlayerStateTest, AbilitiesScaleTheServersValuesAndSetFlags)
{
    Abilities a{};
    a.speed = 2.0f;
    a.gravity = 0.5f;
    a.acceleration = 3.0f;
    a.water_speed = 2.0f;
    a.double_jump = true;
    a.noclip = true;
    a.range = 5;
    const auto header{ header_of(core::player_state::apply(usual_state(7), a)) };
    EXPECT_FLOAT_EQ(header.pos_x2, 600.0f);
    EXPECT_FLOAT_EQ(header.pos_y2, 300.0f);
    EXPECT_FLOAT_EQ(header.pos_x, 3000.0f);
    EXPECT_FLOAT_EQ(header.float_var, 250.0f);
    EXPECT_EQ(header.int_data & (DOUBLE_JUMP | WALK_THROUGH_BLOCKS), DOUBLE_JUMP | WALK_THROUGH_BLOCKS);
    EXPECT_EQ(header.pad[1], 0x85);
    EXPECT_EQ(header.pad[2], 0x85);
    EXPECT_EQ(header.net_id, 7u);
}

TEST(PlayerStateTest, DefaultAbilitiesChangeNothingAndOtherPacketsStayAsTheyAre)
{
    const auto state{ usual_state(7) };
    EXPECT_FALSE(Abilities{}.changes_state());
    EXPECT_EQ(core::player_state::apply(state, Abilities{}), state);
    const auto report{ state_report(0, 1.0f, 2.0f) };
    Abilities fast{};
    fast.speed = 5.0f;
    EXPECT_EQ(core::player_state::apply(report, fast), report);
}

TEST(PlayerStateTest, ValuesAreKeptWithinLimits)
{
    Abilities a{};
    a.speed = 1000.0f;
    a.gravity = 0.0f;
    a.range = 999;
    a.acceleration = std::numeric_limits<float>::quiet_NaN();
    const auto c{ clamped(a) };
    EXPECT_FLOAT_EQ(c.speed, MAX_FACTOR);
    EXPECT_FLOAT_EQ(c.gravity, MIN_FACTOR);
    EXPECT_EQ(c.range, MAX_RANGE);
    EXPECT_FLOAT_EQ(c.acceleration, 1.0f);
    Abilities far{};
    far.range = 100;
    EXPECT_EQ(header_of(core::player_state::apply(usual_state(1), far)).pad[2], 0x80 + 100);
}

TEST(PlayerStateTest, DamageIsTakenOutOfStateReports)
{
    const auto hurt{ state_report(FIRE_DAMAGE | DAMAGE | ACID_DAMAGE | KILLED | (1u << 5), 64.0f, 96.0f) };
    const auto healed{ without_damage(hurt) };
    ASSERT_TRUE(healed.has_value());
    const auto header{ header_of(*healed) };
    EXPECT_EQ(static_cast<std::uint32_t>(header.flags.value), 1u << 5);
    EXPECT_FLOAT_EQ(header.pos_x, 64.0f);
    EXPECT_FALSE(without_damage(state_report(1u << 5, 0.0f, 0.0f)).has_value());
}

TEST(PlayerStateTest, GrowtopiasOwnServersAreKnown)
{
    EXPECT_TRUE(official_address("www.growtopia1.com"));
    EXPECT_TRUE(official_address("WWW.GROWTOPIA2.COM"));
    EXPECT_TRUE(official_address("213.179.209.168"));
    EXPECT_TRUE(official_address("213.179.208.10:17198"));
    EXPECT_FALSE(official_address("127.0.0.2"));
    EXPECT_FALSE(official_address("192.168.1.20"));
    EXPECT_FALSE(official_address("my-gtps.example.net"));
}

TEST(PlayerStateTest, TheGameServerTheProxyPlaysOnDecides)
{
    // config.json may say www.growtopia1.com while the packets go to your
    // own server: only where the proxy connected counts.
    const auto local{ judge(std::make_pair(std::string{ "127.0.0.1" }, std::uint16_t{ 17091 })) };
    EXPECT_FALSE(local.official);
    EXPECT_EQ(local.game_server, "127.0.0.1:17091");
    EXPECT_NE(local.why.find("your own server: the proxy plays on 127.0.0.1:17091"), std::string::npos);

    EXPECT_FALSE(judge(std::make_pair(std::string{ "192.168.1.20" }, std::uint16_t{ 17091 })).official);
    EXPECT_FALSE(judge(std::make_pair(std::string{ "gtps.example.net" }, std::uint16_t{ 17091 })).official);

    const auto ubisoft{ judge(std::make_pair(std::string{ "213.179.209.168" }, std::uint16_t{ 17198 })) };
    EXPECT_TRUE(ubisoft.official);
    EXPECT_NE(ubisoft.why.find("213.179.209.168:17198 is one of Growtopia's own"), std::string::npos);

    const auto none{ judge(std::nullopt) };
    EXPECT_FALSE(none.official);
    EXPECT_TRUE(none.game_server.empty());
}
