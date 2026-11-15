#include "player_state.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>

#include <fmt/format.h>

namespace core::player_state {
namespace {
constexpr std::size_t HEADER_AT{ sizeof(std::uint32_t) };

void put_header(std::vector<std::byte>& data, const packet::GameUpdatePacket& header)
{
    std::memcpy(data.data() + HEADER_AT, &header, sizeof(header));
}

float factor(const float value)
{
    if (!std::isfinite(value)) {
        return 1.0f;
    }
    return std::clamp(value, MIN_FACTOR, MAX_FACTOR);
}

std::uint8_t widened(const std::uint8_t range, const int extra)
{
    return static_cast<std::uint8_t>(std::min(255, static_cast<int>(range) + extra));
}
}

bool Abilities::changes_state() const
{
    return speed != 1.0f || gravity != 1.0f || acceleration != 1.0f || water_speed != 1.0f || double_jump || noclip || range != 0;
}

std::optional<std::uint32_t> message_type(const std::span<const std::byte> data)
{
    if (data.size() < HEADER_AT) {
        return std::nullopt;
    }
    std::uint32_t type{};
    std::memcpy(&type, data.data(), sizeof(type));
    return type;
}

std::optional<packet::GameUpdatePacket> game_header(const std::span<const std::byte> data)
{
    if (data.size() < HEADER_AT + sizeof(packet::GameUpdatePacket) || message_type(data) != packet::NET_MESSAGE_GAME_PACKET) {
        return std::nullopt;
    }
    packet::GameUpdatePacket header{};
    std::memcpy(&header, data.data() + HEADER_AT, sizeof(header));
    return header;
}

Abilities clamped(Abilities a)
{
    a.speed = factor(a.speed);
    a.gravity = factor(a.gravity);
    a.acceleration = factor(a.acceleration);
    a.water_speed = factor(a.water_speed);
    a.range = std::clamp(a.range, 0, MAX_RANGE);
    return a;
}

std::vector<std::byte> apply(std::vector<std::byte> data, const Abilities& a)
{
    auto header{ game_header(data) };
    if (!header || header->type != packet::PACKET_SET_CHARACTER_STATE) {
        return data;
    }
    header->pos_x *= a.acceleration;
    header->pos_x2 *= a.speed;
    header->pos_y2 *= a.gravity;
    header->float_var *= a.water_speed;
    if (a.double_jump) {
        header->int_data |= DOUBLE_JUMP;
    }
    if (a.noclip) {
        header->int_data |= WALK_THROUGH_BLOCKS;
    }
    if (a.range > 0) {
        header->pad[1] = widened(header->pad[1], a.range);
        header->pad[2] = widened(header->pad[2], a.range);
    }
    put_header(data, *header);
    return data;
}

std::vector<std::byte> usual_state(const std::int32_t net_id)
{
    packet::GameUpdatePacket header{};
    header.type = packet::PACKET_SET_CHARACTER_STATE;
    header.pad[0] = 0x00;
    header.pad[1] = 0x80;               // build range
    header.pad[2] = 0x80;               // punch range
    header.net_id = static_cast<std::uint32_t>(net_id);
    header.float_var = 125.0f;          // speed in water
    header.int_data = 0;                // abilities
    header.pos_x = 1000.0f;             // acceleration
    header.pos_y = 100.0f;
    header.pos_x2 = 300.0f;             // walking speed
    header.pos_y2 = 600.0f;             // gravity
    std::vector<std::byte> data(HEADER_AT + sizeof(header));
    const std::uint32_t type{ packet::NET_MESSAGE_GAME_PACKET };
    std::memcpy(data.data(), &type, sizeof(type));
    put_header(data, header);
    return data;
}

std::optional<std::vector<std::byte>> without_damage(const std::span<const std::byte> data)
{
    auto header{ game_header(data) };
    if (!header || header->type != packet::PACKET_STATE) {
        return std::nullopt;
    }
    const auto flags{ static_cast<std::uint32_t>(header->flags.value) };
    if ((flags & HURT) == 0) {
        return std::nullopt;
    }
    header->flags.value = static_cast<packet::PacketFlag>(flags & ~HURT);
    std::vector<std::byte> copy{ data.begin(), data.end() };
    put_header(copy, *header);
    return copy;
}

bool official_address(const std::string_view address)
{
    std::string host{ address };
    std::ranges::transform(host, host.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (const auto colon{ host.find(':') }; colon != std::string::npos) {
        host.resize(colon);
    }
    return host.ends_with("growtopia1.com") || host.ends_with("growtopia2.com")
        || host.starts_with("213.179.208.") || host.starts_with("213.179.209.");
}

Verdict judge(const std::optional<std::pair<std::string, std::uint16_t>>& connected)
{
    if (!connected) {
        return { false, {}, "not connected to a game server yet: decided by the one the proxy connects to" };
    }
    const auto where{ fmt::format("{}:{}", connected->first, connected->second) };
    if (official_address(connected->first)) {
        return { true, where, fmt::format(
            "the game server {} is one of Growtopia's own (213.179.208-209.x, growtopia1/2.com): there these would be cheats against other players", where) };
    }
    return { false, where, fmt::format("your own server: the proxy plays on {}", where) };
}
}
