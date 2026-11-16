#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "modules/packet_view.hpp"
#include "packet/packet_variant.hpp"

using namespace modules::packet_view;

namespace {
std::vector<std::byte> text_packet(const packet::NetMessageType type, const std::string& text)
{
    std::vector<std::byte> data(sizeof(std::uint32_t));
    const std::uint32_t message{ type };
    std::memcpy(data.data(), &message, sizeof(message));
    const auto* begin{ reinterpret_cast<const std::byte*>(text.data()) };
    data.insert(data.end(), begin, begin + text.size());
    data.push_back(std::byte{ 0 });
    return data;
}

std::vector<std::byte> game_packet(const packet::GameUpdatePacket& header, const std::vector<std::byte>& extra = {})
{
    std::vector<std::byte> data(sizeof(std::uint32_t) + sizeof(header));
    const std::uint32_t message{ packet::NET_MESSAGE_GAME_PACKET };
    std::memcpy(data.data(), &message, sizeof(message));
    std::memcpy(data.data() + sizeof(message), &header, sizeof(header));
    data.insert(data.end(), extra.begin(), extra.end());
    return data;
}
}

TEST(PacketViewTest, ReadsTextPacketsWithoutTheTrailingZero)
{
    const auto data{ text_packet(packet::NET_MESSAGE_GENERIC_TEXT, "action|respawn_spike\n") };
    const auto view{ describe(data) };
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->message, packet::NET_MESSAGE_GENERIC_TEXT);
    EXPECT_EQ(message_name(view->message), "generic_text");
    EXPECT_FALSE(view->game_packet);
    EXPECT_EQ(view->text, "action|respawn_spike\n");
}

TEST(PacketViewTest, ReadsAGamePacketsNumbersAndTheCallsName)
{
    packet::GameUpdatePacket header{};
    header.type = packet::PACKET_STATE;
    header.net_id = 5;
    header.flags.value = static_cast<packet::PacketFlag>((1u << 6) | (1u << 16));
    header.pos_x = 64.0f;
    header.pos_y = 96.0f;
    const auto view{ describe(game_packet(header)) };
    ASSERT_TRUE(view.has_value());
    EXPECT_TRUE(view->game_packet);
    EXPECT_EQ(type_name(view->header.type), "state");
    EXPECT_EQ(view->header.net_id, 5u);
    EXPECT_FLOAT_EQ(view->header.pos_x, 64.0f);

    packet::GameUpdatePacket call_header{};
    call_header.type = packet::PACKET_CALL_FUNCTION;
    const packet::PacketVariant variant{ std::string{ "OnKilled" }, std::int32_t{ 7 } };
    const auto serialized{ variant.serialize() };
    const auto call{ describe(game_packet(call_header, { serialized.begin(), serialized.end() })) };
    ASSERT_TRUE(call.has_value());
    EXPECT_EQ(call->call, "OnKilled");
    EXPECT_EQ(type_name(call->header.type), "call_function");
}

TEST(PacketViewTest, RebuildsWithTheChangedNumbersAndKeepsTheRest)
{
    packet::GameUpdatePacket header{};
    header.type = packet::PACKET_STATE;
    header.net_id = 5;
    header.flags.value = static_cast<packet::PacketFlag>(0xFFu);
    const std::vector<std::byte> extra{ std::byte{ 1 }, std::byte{ 2 }, std::byte{ 3 } };
    const auto original{ game_packet(header, extra) };

    auto view{ *describe(original) };
    view.header.flags.value = static_cast<packet::PacketFlag>(0u);
    const auto changed{ rebuild(view, original) };
    ASSERT_EQ(changed.size(), original.size());
    const auto again{ describe(changed) };
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(static_cast<std::uint32_t>(again->header.flags.value), 0u);
    EXPECT_EQ(again->header.net_id, 5u);
    EXPECT_TRUE(std::equal(extra.begin(), extra.end(), changed.end() - 3));
}

TEST(PacketViewTest, RebuildsTextPacketsFromTheirText)
{
    const auto original{ text_packet(packet::NET_MESSAGE_GAME_MESSAGE, "action|quit") };
    auto view{ *describe(original) };
    view.text = "action|join_request\nname|START\n";
    const auto changed{ rebuild(view, original) };
    const auto again{ describe(changed) };
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again->text, "action|join_request\nname|START\n");
    EXPECT_EQ(again->message, packet::NET_MESSAGE_GAME_MESSAGE);
    EXPECT_EQ(changed.back(), std::byte{ 0 });
}

TEST(PacketViewTest, ShortAndUnknownPacketsAreSafe)
{
    EXPECT_FALSE(describe(std::vector<std::byte>{ std::byte{ 1 } }).has_value());
    const std::vector<std::byte> short_game{ std::byte{ 4 }, std::byte{ 0 }, std::byte{ 0 }, std::byte{ 0 }, std::byte{ 9 } };
    const auto view{ describe(short_game) };
    ASSERT_TRUE(view.has_value());
    EXPECT_FALSE(view->game_packet);
    EXPECT_EQ(rebuild(*view, short_game), short_game);
}
