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

TEST(LogonPacketTest, ForcingItemsDownloadChangesOnlyTheHash)
{
    // Layout of the official v5.61 logon packet.
    const std::string settings{
        "proto=225|choosemusic=audio/mp3/about_theme.mp3|active_holiday=0|enableInventoryTab=1|bigBackpack=1|seed_diary_hash=3856714732"
    };
    const PacketVariant variant{
        std::string{ "OnSuperMainStartAcceptLogonFB211131ddf" },
        std::uint32_t{ 3735928559u },   // > INT32_MAX, sent unsigned by the server
        std::string{ "ubistatic-a.akamaihd.net" },
        std::string{ "0098/01020126/cache/" },
        std::string{ "cc.cz.madkite.freedom org.aqua.gg" },
        settings,
        std::uint32_t{ 77 }
    };

    game::OnSuperMainStartAcceptLogonHrdxs47254722215a pkt{};
    ASSERT_TRUE(pkt.read(make_server_payload(variant)));
    EXPECT_EQ(static_cast<std::uint32_t>(pkt.item_hash), 3735928559u);
    EXPECT_EQ(pkt.u, "ubistatic-a.akamaihd.net");
    EXPECT_EQ(pkt.uu, "0098/01020126/cache/");
    EXPECT_EQ(pkt.settings, settings);
    EXPECT_EQ(pkt.player_tribute_hash, 77u);

    // Unmodified: byte identical.
    EXPECT_EQ(PacketHelper::serialize(pkt.write()), PacketHelper::serialize(Payload{ make_server_payload(variant) }));

    // Forcing an items.dat download: only [1] changes, everything else is kept.
    pkt.item_hash = 0;
    const auto written{ pkt.write() };
    const auto& args{ std::get<VariantPayload>(written).variant.get_variants() };
    const auto& original{ variant.get_variants() };
    ASSERT_EQ(args.size(), original.size());
    ASSERT_TRUE(std::holds_alternative<std::uint32_t>(args[1]));
    EXPECT_EQ(std::get<std::uint32_t>(args[1]), 0u);
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i != 1) {
            EXPECT_EQ(args[i], original[i]) << "argument " << i << " changed";
        }
    }
    ASSERT_TRUE(std::holds_alternative<std::string>(args[5]));
    EXPECT_EQ(std::get<std::string>(args[5]), settings);
}

TEST(PacketVariantTest, SetIntegerNeverReplacesStrings)
{
    PacketVariant variant{ std::string{ "f" }, std::int32_t{ 1 }, std::string{ "keep me" }, std::uint32_t{ 2 } };
    EXPECT_TRUE(variant.set_integer(1, 5));
    EXPECT_FALSE(variant.set_integer(2, 0));
    EXPECT_TRUE(variant.set_integer(3, 6));
    EXPECT_FALSE(variant.set_integer(9, 0));

    const auto& args{ variant.get_variants() };
    EXPECT_EQ(std::get<std::int32_t>(args[1]), 5);
    EXPECT_EQ(std::get<std::string>(args[2]), "keep me");
    EXPECT_EQ(std::get<std::uint32_t>(args[3]), 6u);
}

TEST(PacketVariantTest, RefusesUnknownArgumentTypes)
{
    // count=2: [0] string "ab", [1] type 7 (unknown)
    const std::vector<std::byte> data{
        std::byte{ 2 },
        std::byte{ 0 }, std::byte{ 2 }, std::byte{ 2 }, std::byte{ 0 }, std::byte{ 0 }, std::byte{ 0 }, std::byte{ 'a' }, std::byte{ 'b' },
        std::byte{ 1 }, std::byte{ 7 }, std::byte{ 1 }, std::byte{ 2 }, std::byte{ 3 }, std::byte{ 4 },
    };
    PacketVariant variant{};
    EXPECT_FALSE(variant.deserialize(data));
    EXPECT_EQ(variant.size(), 0u);
}

TEST(PacketVariantTest, RefusesTruncatedData)
{
    const std::vector<std::byte> data{ std::byte{ 1 }, std::byte{ 0 }, std::byte{ 9 }, std::byte{ 1 } };
    PacketVariant variant{};
    EXPECT_FALSE(variant.deserialize(data));
}
