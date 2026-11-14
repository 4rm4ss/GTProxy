#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "../packet_helper.hpp"

namespace packet::game {
struct Disconnect : GamePacket<PacketId::Disconnect, PACKET_DISCONNECT> {
    bool read(const Payload& payload) override
    {
        return is_payload<GamePayload>(payload);
    }

    Payload write() override
    {
        GamePayload game_payload{};
        game_payload.packet.type = PACKET_TYPE;
        game_payload.packet.net_id = -1;
        return game_payload;
    }
};

struct OnSendToServer : VariantPacket<PacketId::OnSendToServer> {
    uint16_t port{};
    int32_t token{};
    int32_t user{};
    std::string address;
    std::string door_id;
    std::string uuid_token;
    uint8_t login_mode{};
    std::string username;

    bool read(const Payload& payload) override
    {
        const auto var{ get_payload_if<VariantPayload>(payload) };
        if (!var) {
            return false;
        }

        variant = var->variant;
        game_packet = var->game_packet;

        if (variant.size() < 5) {
            return false;
        }

        port = static_cast<uint16_t>(get_integer(1));
        token = get_integer(2);
        user = get_integer(3);

        // "address|door_id|uuid_token"
        const auto parts{ split_address(variant.get<std::string>(4)) };
        address = parts.size() > 0 ? parts[0] : std::string{};
        door_id = parts.size() > 1 ? parts[1] : std::string{};
        uuid_token = parts.size() > 2 ? parts[2] : std::string{};

        if (variant.size() > 5) { login_mode = static_cast<uint8_t>(get_integer(5)); }
        if (variant.size() > 6) { username = variant.get<std::string>(6); }
        return true;
    }

    Payload write() override
    {
        // Packet received from the server: write the fields back into the original
        // variant, keeping each argument's type and any argument or address part we
        // do not know about, so newer game versions pass through intact. Unmodified
        // fields produce byte-identical output.
        if (variant.size() >= 5) {
            PacketVariant patched = variant;

            patched.set_integer(1, port);
            patched.set_integer(2, token);
            patched.set_integer(3, user);

            if (std::holds_alternative<std::string>(patched.get_variants()[4])) {
                auto parts{ split_address(variant.get<std::string>(4)) };
                if (parts.empty()) {
                    parts.emplace_back();
                }

                parts[0] = address;
                if (parts.size() > 1 || !door_id.empty() || !uuid_token.empty()) {
                    parts.resize(std::max<std::size_t>(parts.size(), 2));
                    parts[1] = door_id;
                }
                if (parts.size() > 2 || !uuid_token.empty()) {
                    parts.resize(std::max<std::size_t>(parts.size(), 3));
                    parts[2] = uuid_token;
                }

                patched.set(4, join_address(parts));
            }

            patched.set_integer(5, login_mode);
            if (patched.size() > 6 && std::holds_alternative<std::string>(patched.get_variants()[6])) {
                patched.set(6, username);
            }

            return VariantPayload{ game_packet, patched };
        }

        const PacketVariant fresh{
            std::string{ "OnSendToServer" },
            static_cast<int32_t>(port),
            token,
            user,
            join_address({ address, door_id, uuid_token }),
            static_cast<uint32_t>(login_mode),
            username
        };
        return VariantPayload{ game_packet, fresh };
    }

private:
    // Integers may arrive as signed or unsigned variants depending on the server.
    [[nodiscard]] int32_t get_integer(const std::size_t index) const
    {
        if (index >= variant.size()) {
            return 0;
        }

        const auto& value{ variant.get_variants()[index] };
        if (const auto* signed_value{ std::get_if<int32_t>(&value) }) {
            return *signed_value;
        }

        if (const auto* unsigned_value{ std::get_if<uint32_t>(&value) }) {
            return static_cast<int32_t>(*unsigned_value);
        }

        if (const auto* float_value{ std::get_if<float>(&value) }) {
            return static_cast<int32_t>(*float_value);
        }

        return 0;
    }

    [[nodiscard]] static std::vector<std::string> split_address(const std::string& text)
    {
        std::vector<std::string> parts;
        std::size_t begin{ 0 };
        while (true) {
            const auto separator{ text.find('|', begin) };
            parts.emplace_back(text.substr(begin, separator - begin));
            if (separator == std::string::npos) {
                break;
            }
            begin = separator + 1;
        }
        return parts;
    }

    [[nodiscard]] static std::string join_address(const std::vector<std::string>& parts)
    {
        std::string out;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (i > 0) {
                out += '|';
            }
            out += parts[i];
        }
        return out;
    }
};

struct OnSuperMainStartAcceptLogonHrdxs47254722215a : VariantPacket<PacketId::OnSuperMainStartAcceptLogonHrdxs47254722215a> {
    // Actual function name sent by the server (its suffix changes between
    // versions, e.g. ...Hrdxs47254722215a, ...FB211131d, ...FB211131ddf).
    std::string function_name{ "OnSuperMainStartAcceptLogonHrdxs47254722215a" };
    // Arguments as sent by the official server (v5.61):
    int32_t item_hash{};            // [1] items.dat hash (sent unsigned)
    std::string u;                  // [2] CDN host, e.g. ubistatic-a.akamaihd.net
    std::string uu;                 // [3] CDN path, e.g. 0098/xxxxxxxx/cache/
    std::string uuu;                // [4] blocked package list
    std::string settings;           // [5] session settings: proto=..|enableInventoryTab=1|bigBackpack=1|...
    uint32_t player_tribute_hash{}; // [6] player tribute hash

    bool read(const Payload& payload) override
    {
        const auto var{ get_payload_if<VariantPayload>(payload) };
        if (!var) {
            return false;
        }

        variant = var->variant;
        game_packet = var->game_packet;

        if (variant.size() < 2) {
            return false;
        }

        const auto string_at = [this](const std::size_t index) {
            const auto& values{ variant.get_variants() };
            const auto* value{ index < values.size() ? std::get_if<std::string>(&values[index]) : nullptr };
            return value ? *value : std::string{};
        };

        function_name = string_at(0);
        item_hash = static_cast<int32_t>(variant.get_integer(1).value_or(0));
        u = string_at(2);
        uu = string_at(3);
        uuu = string_at(4);
        settings = string_at(5);
        player_tribute_hash = static_cast<uint32_t>(variant.get_integer(6).value_or(0));

        return true;
    }

    Payload write() override
    {
        // Received from the server: patch only the items.dat hash and keep every
        // other argument exactly as sent (the settings string in particular
        // carries what the client needs, e.g. enableInventoryTab/bigBackpack).
        if (variant.size() >= 2) {
            PacketVariant patched = variant;
            patched.set_integer(1, static_cast<uint32_t>(item_hash));
            return VariantPayload{ game_packet, patched };
        }

        const PacketVariant fresh{
            function_name,
            static_cast<uint32_t>(item_hash),
            u,
            uu,
            uuu,
            settings,
            player_tribute_hash
        };
        return VariantPayload{ game_packet, fresh };
    }
};
}
