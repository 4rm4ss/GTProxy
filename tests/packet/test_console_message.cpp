#include <string>
#include <variant>

#include <gtest/gtest.h>

#include "packet/message/chat.hpp"

using namespace packet;

TEST(ConsoleMessageTest, ProxyMessagesUseOnConsoleMessage)
{
    message::Log log{};
    log.msg = "`2Hello from the proxy";

    const auto payload{ log.write() };
    const auto* var{ std::get_if<VariantPayload>(&payload) };
    ASSERT_NE(var, nullptr);
    EXPECT_EQ(var->game_packet.type, PACKET_CALL_FUNCTION);
    ASSERT_EQ(var->variant.size(), 2u);
    EXPECT_EQ(var->variant.get<std::string>(0), "OnConsoleMessage");
    EXPECT_EQ(var->variant.get<std::string>(1), "`2Hello from the proxy");

    // Serialized as a game packet (NET_MESSAGE_GAME_PACKET = 4).
    const auto bytes{ PacketHelper::serialize(payload) };
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], std::byte{ 4 });
}
