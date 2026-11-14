#pragma once
#include "../packet_helper.hpp"

namespace packet::game {
struct OnNameChanged : VariantPacket<PacketId::OnNameChanged> {
    int32_t net_id;
    std::string name;
    // Game 5.x: second argument is the title/wrench JSON (the same text as
    // "titleIcon" in OnSpawn). Without it the client shows an empty name.
    std::string title;

    OnNameChanged()
        : net_id{ -1 }
    {

    }

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

        net_id = static_cast<int32_t>(game_packet.net_id);
        name = variant.get<std::string>(1);
        if (variant.size() > 2 && std::holds_alternative<std::string>(variant.get_variants()[2])) {
            title = variant.get<std::string>(2);
        }
        return true;
    }

    Payload write() override
    {
        GameUpdatePacket header{ game_packet };
        header.type = PACKET_CALL_FUNCTION;
        header.net_id = static_cast<std::uint32_t>(net_id);

        PacketVariant out{ std::string{ "OnNameChanged" }, name };
        if (!title.empty()) {
            out.add(title);
        }

        // Keep any further arguments a newer server sent.
        const auto& original{ variant.get_variants() };
        for (std::size_t i{ 3 }; i < original.size(); ++i) {
            out.add(original[i]);
        }
        return VariantPayload{ header, out };
    }
};

struct OnChangeSkin : VariantPacket<PacketId::OnChangeSkin> {
    int32_t net_id;
    uint32_t skin_code;

    OnChangeSkin()
        : net_id{ -1 }
        , skin_code{ 0 }
    {

    }

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

        skin_code = variant.get<uint32_t>(1);
        return true;
    }

    Payload write() override
    {
        game_packet.net_id = net_id;
        const PacketVariant variant{
            "OnChangeSkin",
            skin_code
        };
        return VariantPayload{ game_packet, variant };
    }
};
}
namespace packet::game {
// Movement/state update. The game sends one for the local player many times
// per second (position in pixels: pos_x / pos_y, 32 px per tile).
struct State : GamePacket<PacketId::State, PACKET_STATE> {
    bool read(const Payload& payload) override
    {
        const auto game{ get_payload_if<GamePayload>(payload) };
        if (!game) {
            return false;
        }

        game_packet = game->packet;
        return true;
    }

    Payload write() override
    {
        GamePayload game_payload{};
        game_payload.packet = game_packet;
        return game_payload;
    }
};
}
