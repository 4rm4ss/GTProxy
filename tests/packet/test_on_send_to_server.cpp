#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "packet/game/server.hpp"

using namespace packet;

namespace {
VariantPayload make_server_payload(const PacketVariant& variant)
{
    GameUpdatePacket game_packet{};
    game_packet.type = PACKET_CALL_FUNCTION;
    game_packet.net_id = static_cast<std::uint32_t>(-1);
    return VariantPayload{ game_packet, variant };
}

// Shape of the packet the official server sends (integers as signed int32).
PacketVariant official_like_variant()
{
    return PacketVariant{
        std::string{ "OnSendToServer" },
        std::int32_t{ 17092 },
        std::int32_t{ 123456 },
        std::int32_t{ 987654 },
        std::string{ "213.179.209.175|0|uuid-token-abc" },
        std::int32_t{ 1 },
        std::string{ "PlayerName" }
    };
}
}

TEST(OnSendToServerTest, ReadsFields)
{
    game::OnSendToServer pkt{};
    ASSERT_TRUE(pkt.read(make_server_payload(official_like_variant())));

    EXPECT_EQ(pkt.port, 17092);
    EXPECT_EQ(pkt.token, 123456);
    EXPECT_EQ(pkt.user, 987654);
    EXPECT_EQ(pkt.address, "213.179.209.175");
    EXPECT_EQ(pkt.door_id, "0");
    EXPECT_EQ(pkt.uuid_token, "uuid-token-abc");
    EXPECT_EQ(pkt.login_mode, 1);
    EXPECT_EQ(pkt.username, "PlayerName");
}

TEST(OnSendToServerTest, UnmodifiedRoundTripIsByteIdentical)
{
    const auto original{ make_server_payload(official_like_variant()) };

    game::OnSendToServer pkt{};
    ASSERT_TRUE(pkt.read(original));

    EXPECT_EQ(PacketHelper::serialize(pkt.write()), PacketHelper::serialize(Payload{ original }));
}

TEST(OnSendToServerTest, RedirectChangesOnlyPortAndAddress)
{
    game::OnSendToServer pkt{};
    ASSERT_TRUE(pkt.read(make_server_payload(official_like_variant())));

    pkt.address = "127.0.0.1";
    pkt.port = 16999;

    const auto written{ pkt.write() };
    const auto* var{ std::get_if<VariantPayload>(&written) };
    ASSERT_NE(var, nullptr);

    const auto& args{ var->variant.get_variants() };
    ASSERT_EQ(args.size(), 7u);
    EXPECT_EQ(std::get<std::string>(args[0]), "OnSendToServer");
    EXPECT_EQ(std::get<std::int32_t>(args[1]), 16999);
    EXPECT_EQ(std::get<std::int32_t>(args[2]), 123456);
    EXPECT_EQ(std::get<std::int32_t>(args[3]), 987654);
    EXPECT_EQ(std::get<std::string>(args[4]), "127.0.0.1|0|uuid-token-abc");
    // login mode keeps its original (signed) type and value
    EXPECT_EQ(std::get<std::int32_t>(args[5]), 1);
    EXPECT_EQ(std::get<std::string>(args[6]), "PlayerName");
}

TEST(OnSendToServerTest, KeepsUnsignedTypesAndExtraArguments)
{
    const PacketVariant variant{
        std::string{ "OnSendToServer" },
        std::uint32_t{ 17093 },
        std::int32_t{ 5 },
        std::int32_t{ 6 },
        std::string{ "1.2.3.4|DOOR|uuid|extra-part" },
        std::uint32_t{ 3 },
        std::string{ "Name" },
        std::string{ "future-argument" }
    };

    game::OnSendToServer pkt{};
    ASSERT_TRUE(pkt.read(make_server_payload(variant)));
    EXPECT_EQ(pkt.port, 17093);
    EXPECT_EQ(pkt.login_mode, 3);

    pkt.address = "127.0.0.1";
    pkt.port = 16999;

    const auto written{ pkt.write() };
    const auto& args{ std::get<VariantPayload>(written).variant.get_variants() };
    ASSERT_EQ(args.size(), 8u);
    EXPECT_EQ(std::get<std::uint32_t>(args[1]), 16999u);
    EXPECT_EQ(std::get<std::string>(args[4]), "127.0.0.1|DOOR|uuid|extra-part");
    EXPECT_EQ(std::get<std::uint32_t>(args[5]), 3u);
    EXPECT_EQ(std::get<std::string>(args[7]), "future-argument");
}

TEST(OnSendToServerTest, RejectsTooFewArguments)
{
    const PacketVariant variant{ std::string{ "OnSendToServer" }, std::int32_t{ 1 } };

    game::OnSendToServer pkt{};
    EXPECT_FALSE(pkt.read(make_server_payload(variant)));
}

TEST(OnSendToServerTest, FreshPacketFromScriptsStillWorks)
{
    game::OnSendToServer pkt{};
    pkt.port = 17091;
    pkt.token = 1;
    pkt.user = 2;
    pkt.address = "127.0.0.1";
    pkt.door_id = "0";
    pkt.uuid_token = "u";
    pkt.login_mode = 1;
    pkt.username = "n";

    const auto written{ pkt.write() };
    const auto& args{ std::get<VariantPayload>(written).variant.get_variants() };
    ASSERT_EQ(args.size(), 7u);
    EXPECT_EQ(std::get<std::string>(args[0]), "OnSendToServer");
    EXPECT_EQ(std::get<std::int32_t>(args[1]), 17091);
    EXPECT_EQ(std::get<std::string>(args[4]), "127.0.0.1|0|u");
}
