#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "../packet/packet_types.hpp"

// A packet on the wire as a module sees it: which message it is, the game
// packet's header (the numbers every game packet carries) and the text or
// the function name it holds. Free of the network so it can be tested
// (tests/modules/test_packet_view.cpp).
namespace modules::packet_view {
struct View {
    std::uint32_t message{ packet::NET_MESSAGE_UNKNOWN };
    bool game_packet{ false };
    packet::GameUpdatePacket header{};
    std::string text;       // NET_MESSAGE_GENERIC_TEXT / GAME_MESSAGE
    std::string call;       // PACKET_CALL_FUNCTION: the function's name
};

// The message's name as modules use it ("generic_text", "game_packet" ...).
[[nodiscard]] std::string_view message_name(std::uint32_t message);
// The game packet's type as modules use it ("state", "call_function" ...).
[[nodiscard]] std::string type_name(packet::PacketType type);

[[nodiscard]] std::optional<View> describe(std::span<const std::byte> data);
// The packet again with the view's header or text; everything else (the
// extra data of a game packet) is kept from `original`.
[[nodiscard]] std::vector<std::byte> rebuild(const View& view, std::span<const std::byte> original);
}
