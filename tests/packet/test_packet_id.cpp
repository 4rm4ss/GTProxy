#include <string>

#include <gtest/gtest.h>

#include "packet/packet_id.hpp"

using namespace packet;

namespace {
VariantPayload make_call(const std::string& function_name)
{
    GameUpdatePacket game_packet{};
    game_packet.type = PACKET_CALL_FUNCTION;
    return VariantPayload{ game_packet, PacketVariant{ function_name, 123 } };
}
}

TEST(PacketIdTest, ResolvesKnownVariantFunction)
{
    EXPECT_EQ(derive_packet_id(make_call("OnSendToServer")), PacketId::OnSendToServer);
}

TEST(PacketIdTest, ResolvesCurrentLogonFunctionName)
{
    EXPECT_EQ(
        derive_packet_id(make_call("OnSuperMainStartAcceptLogonHrdxs47254722215a")),
        PacketId::OnSuperMainStartAcceptLogonHrdxs47254722215a
    );
}

TEST(PacketIdTest, ResolvesLogonFunctionWithChangedSuffix)
{
    EXPECT_EQ(
        derive_packet_id(make_call("OnSuperMainStartAcceptLogonFB596A24")),
        PacketId::OnSuperMainStartAcceptLogonHrdxs47254722215a
    );
}

TEST(PacketIdTest, UnknownFunctionStaysUnknown)
{
    EXPECT_EQ(derive_packet_id(make_call("OnSomethingNew")), PacketId::Unknown);
}
