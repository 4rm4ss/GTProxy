#include "packet_view.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

#include <magic_enum/magic_enum.hpp>

#include "../packet/packet_variant.hpp"

namespace modules::packet_view {
namespace {
constexpr std::size_t TYPE_SIZE{ sizeof(std::uint32_t) };

std::string lowered(const std::string_view name, const std::string_view prefix)
{
    std::string out{ name.starts_with(prefix) ? name.substr(prefix.size()) : name };
    std::ranges::transform(out, out.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}
}

std::string_view message_name(const std::uint32_t message)
{
    switch (message) {
    case packet::NET_MESSAGE_SERVER_HELLO: return "hello";
    case packet::NET_MESSAGE_GENERIC_TEXT: return "generic_text";
    case packet::NET_MESSAGE_GAME_MESSAGE: return "game_message";
    case packet::NET_MESSAGE_GAME_PACKET: return "game_packet";
    case packet::NET_MESSAGE_ERROR: return "error";
    case packet::NET_MESSAGE_TRACK: return "track";
    default: return "other";
    }
}

std::string type_name(const packet::PacketType type)
{
    return lowered(magic_enum::enum_name(type), "PACKET_");
}

std::optional<View> describe(const std::span<const std::byte> data)
{
    if (data.size() < TYPE_SIZE) {
        return std::nullopt;
    }
    View view{};
    std::memcpy(&view.message, data.data(), TYPE_SIZE);

    const auto payload{ data.subspan(TYPE_SIZE) };
    if (view.message == packet::NET_MESSAGE_GENERIC_TEXT || view.message == packet::NET_MESSAGE_GAME_MESSAGE) {
        const auto* begin{ reinterpret_cast<const char*>(payload.data()) };
        std::size_t length{ payload.size() };
        while (length > 0 && begin[length - 1] == '\0') {
            --length;
        }
        view.text.assign(begin, length);
        return view;
    }

    if (view.message != packet::NET_MESSAGE_GAME_PACKET || payload.size() < sizeof(packet::GameUpdatePacket)) {
        return view;
    }
    view.game_packet = true;
    std::memcpy(&view.header, payload.data(), sizeof(view.header));

    if (view.header.type == packet::PACKET_CALL_FUNCTION) {
        packet::PacketVariant variant{};
        if (variant.deserialize(payload.subspan(sizeof(view.header))) && variant.size() > 0) {
            view.call = variant.get<std::string>(0);
        }
    }
    return view;
}

std::vector<std::byte> rebuild(const View& view, const std::span<const std::byte> original)
{
    if (view.message == packet::NET_MESSAGE_GENERIC_TEXT || view.message == packet::NET_MESSAGE_GAME_MESSAGE) {
        std::vector<std::byte> data(TYPE_SIZE);
        std::memcpy(data.data(), &view.message, TYPE_SIZE);
        const auto* begin{ reinterpret_cast<const std::byte*>(view.text.data()) };
        data.insert(data.end(), begin, begin + view.text.size());
        data.push_back(std::byte{ 0 });
        return data;
    }

    std::vector<std::byte> data{ original.begin(), original.end() };
    if (view.game_packet && data.size() >= TYPE_SIZE + sizeof(packet::GameUpdatePacket)) {
        std::memcpy(data.data() + TYPE_SIZE, &view.header, sizeof(view.header));
    }
    return data;
}
}
