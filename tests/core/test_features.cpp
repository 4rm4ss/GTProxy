#include <string>
#include <variant>

#include <gtest/gtest.h>

#include "core/features.hpp"
#include "packet/packet_id.hpp"
#include "packet/packet_registry.hpp"
#include "packet/register_packets.hpp"
#include "packet/game/dialog.hpp"
#include "packet/game/player.hpp"
#include "packet/message/dialog.hpp"
#include "packet/message/exit.hpp"

using namespace core::features;

TEST(FeaturesTest, WorldNames)
{
    EXPECT_EQ(normalize_world_name("start"), "START");
    EXPECT_EQ(normalize_world_name("Buy4Sale2"), "BUY4SALE2");
    EXPECT_EQ(normalize_world_name(std::string(23, 'a')), std::string(23, 'A'));
    EXPECT_FALSE(normalize_world_name(""));
    EXPECT_FALSE(normalize_world_name(std::string(24, 'a')));
    EXPECT_FALSE(normalize_world_name("my world"));
    EXPECT_FALSE(normalize_world_name("WORLD|DOOR"));
    EXPECT_FALSE(normalize_world_name("wörld"));

    EXPECT_EQ(normalize_door_id("shop_1"), "SHOP_1");
    EXPECT_FALSE(normalize_door_id(""));
    EXPECT_FALSE(normalize_door_id("a|b"));
}

TEST(FeaturesTest, ReplaceFieldKeepsEveryOtherLine)
{
    const std::string spawn{
        "spawn|avatar\nnetID|7\nuserID|123\neid|abc\nip|x\ncolrect|0|0|20|30\nposXY|1600|736\n"
        "name|`wPlayer``\ntitleIcon|{\"PlayerWorldID\":7}\ncountry|ru\ninvis|0\nmstate|0\nsmstate|0\n"
        "onlineID|\nnewField|42\ntype|local"
    };

    const auto replaced{ replace_field(spawn, "name", "`wNick`` `4[7]``") };
    ASSERT_TRUE(replaced);

    std::string expected{ spawn };
    expected.replace(expected.find("`wPlayer``"), std::string{ "`wPlayer``" }.size(), "`wNick`` `4[7]``");
    EXPECT_EQ(*replaced, expected);

    EXPECT_EQ(field_value(*replaced, "name"), "`wNick`` `4[7]``");
    EXPECT_EQ(field_value(spawn, "titleIcon"), "{\"PlayerWorldID\":7}");
    EXPECT_EQ(field_value(spawn, "type"), "local");
    EXPECT_EQ(field_value(spawn, "onlineID"), "");
    EXPECT_FALSE(field_value(spawn, "nam"));
    EXPECT_FALSE(replace_field(spawn, "missing", "x"));
    // A key is matched as a whole: "netID" must not match "netIDx|".
    EXPECT_EQ(replace_field("netIDx|1\nnetID|2", "netID", "9"), "netIDx|1\nnetID|9");
}

TEST(FeaturesTest, NamesAndSanitizing)
{
    EXPECT_EQ(strip_colors("`2Hello`` `4[5]``"), "Hello [5]");
    EXPECT_EQ(sanitize("a|b\nc\rd"), "abcd");
    EXPECT_EQ(colored_name("Bob"), "`wBob``");
    EXPECT_EQ(colored_name("`2Bob``"), "`2Bob``");
    EXPECT_EQ(colored_name("Bo|b"), "`wBob``");
}

TEST(FeaturesTest, DialogReturnIsDecodedAndFieldsJoined)
{
    packet::register_all_packets();

    const std::string text{
        "action|dialog_return\ndialog_name|gtproxy_menu\nworld|START|SHOP\nnick|Bob\nshow_netid|1\nbuttonClicked|warp\n"
    };
    const packet::TextPayload payload{ packet::NET_MESSAGE_GENERIC_TEXT, utils::TextParse{ text } };
    EXPECT_EQ(packet::derive_packet_id(payload), packet::PacketId::DialogReturn);

    const auto pkt{ packet::PacketRegistry::instance().create(payload) };
    ASSERT_NE(pkt, nullptr);
    ASSERT_EQ(pkt->id(), packet::PacketId::DialogReturn);
    const auto& dialog{ static_cast<packet::message::DialogReturn&>(*pkt) };
    EXPECT_EQ(dialog.dialog_name, "gtproxy_menu");
    EXPECT_EQ(dialog.button_clicked, "warp");
    EXPECT_EQ(dialog.field("world"), "START|SHOP");
    EXPECT_EQ(dialog.field("nick"), "Bob");
    EXPECT_TRUE(dialog.checked("show_netid"));
    EXPECT_FALSE(dialog.checked("missing"));
}

TEST(FeaturesTest, MultiLineTextPacketsAreRecognised)
{
    const auto id = [](const std::string& text) {
        return packet::derive_packet_id(packet::TextPayload{ packet::NET_MESSAGE_GAME_MESSAGE, utils::TextParse{ text } });
    };

    EXPECT_EQ(id("action|join_request\nname|START\ninvitedWorld|0"), packet::PacketId::JoinRequest);
    EXPECT_EQ(id("action|quit_to_exit"), packet::PacketId::QuitToExit);
    EXPECT_EQ(id("action|quit"), packet::PacketId::Quit);
    EXPECT_EQ(id("action|input\n|text|hi"), packet::PacketId::Input);
    EXPECT_EQ(id("action|dialog_returned"), packet::PacketId::Unknown);
}

TEST(FeaturesTest, JoinRequestWithDoor)
{
    packet::message::JoinRequest join{};
    join.world_name = "START|SHOP";
    join.invited_world = false;
    const auto payload{ join.write() };
    const auto* text{ std::get_if<packet::TextPayload>(&payload) };
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->data.get_raw(), "action|join_request\nname|START|SHOP\ninvitedWorld|0");
}

TEST(FeaturesTest, OnNameChangedCarriesTitleJson)
{
    packet::game::OnNameChanged pkt{};
    pkt.net_id = 7;
    pkt.name = "`wNick``";
    pkt.title = R"({"PlayerWorldID":7})";

    const auto payload{ pkt.write() };
    const auto* var{ std::get_if<packet::VariantPayload>(&payload) };
    ASSERT_NE(var, nullptr);
    EXPECT_EQ(var->game_packet.net_id, 7u);
    ASSERT_EQ(var->variant.size(), 3u);
    EXPECT_EQ(var->variant.get<std::string>(0), "OnNameChanged");
    EXPECT_EQ(var->variant.get<std::string>(1), "`wNick``");
    EXPECT_EQ(var->variant.get<std::string>(2), R"({"PlayerWorldID":7})");

    // Reading what the server sent keeps the title and extra arguments.
    packet::GameUpdatePacket header{};
    header.type = packet::PACKET_CALL_FUNCTION;
    header.net_id = 9;
    const packet::VariantPayload incoming{
        header,
        packet::PacketVariant{ std::string{ "OnNameChanged" }, std::string{ "`2Real``" }, std::string{ "{\"t\":1}" }, std::int32_t{ 5 } }
    };
    packet::game::OnNameChanged read{};
    ASSERT_TRUE(read.read(incoming));
    EXPECT_EQ(read.net_id, 9);
    EXPECT_EQ(read.name, "`2Real``");
    EXPECT_EQ(read.title, "{\"t\":1}");

    read.name = "`wNick``";
    const auto rewritten{ read.write() };
    const auto* out{ std::get_if<packet::VariantPayload>(&rewritten) };
    ASSERT_NE(out, nullptr);
    ASSERT_EQ(out->variant.size(), 4u);
    EXPECT_EQ(out->variant.get<std::string>(1), "`wNick``");
    EXPECT_EQ(out->variant.get<std::string>(2), "{\"t\":1}");
    EXPECT_EQ(out->variant.get<std::int32_t>(3), 5);
    EXPECT_EQ(out->game_packet.net_id, 9u);
}

TEST(FeaturesTest, DialogRequestPacket)
{
    packet::game::OnDialogRequest dialog{};
    dialog.dialog = "add_button|warp|Warp|noflags|0|0|\nend_dialog|gtproxy_menu|Close|Apply|\n";
    const auto payload{ dialog.write() };
    const auto* var{ std::get_if<packet::VariantPayload>(&payload) };
    ASSERT_NE(var, nullptr);
    EXPECT_EQ(var->game_packet.type, packet::PACKET_CALL_FUNCTION);
    EXPECT_EQ(var->game_packet.net_id, static_cast<std::uint32_t>(-1));
    ASSERT_EQ(var->variant.size(), 2u);
    EXPECT_EQ(var->variant.get<std::string>(0), "OnDialogRequest");
    EXPECT_EQ(var->variant.get<std::string>(1), dialog.dialog);
}
