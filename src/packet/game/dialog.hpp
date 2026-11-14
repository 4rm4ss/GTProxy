#pragma once
#include <string>

#include "../packet_types.hpp"
#include "../packet_id.hpp"
#include "../packet_helper.hpp"

namespace packet::game {
// Shows a dialog in the game. The text uses the game's dialog markup
// (add_label_with_icon|..., add_button|..., end_dialog|<name>|...).
struct OnDialogRequest : VariantPacket<PacketId::OnDialogRequest> {
    std::string dialog;

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

        dialog = variant.get<std::string>(1);
        return true;
    }

    Payload write() override
    {
        GameUpdatePacket header{};
        header.type = PACKET_CALL_FUNCTION;
        header.net_id = static_cast<std::uint32_t>(-1);
        return VariantPayload{ header, PacketVariant{ std::string{ "OnDialogRequest" }, dialog } };
    }
};

// The server sends this after the player left a world (world select menu).
struct OnRequestWorldSelectMenu : VariantPacket<PacketId::OnRequestWorldSelectMenu> {
    bool read(const Payload& payload) override
    {
        const auto var{ get_payload_if<VariantPayload>(payload) };
        if (!var) {
            return false;
        }

        variant = var->variant;
        game_packet = var->game_packet;
        return true;
    }

    Payload write() override
    {
        return VariantPayload{ game_packet, variant };
    }
};
}
