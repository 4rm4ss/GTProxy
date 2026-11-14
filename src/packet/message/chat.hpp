#pragma once
#include "../packet_helper.hpp"

namespace packet::message {
struct Log : TextPacket<PacketId::Log> {
    std::string msg;

    bool read(const Payload& payload) override
    {
        const auto text{ get_payload_if<TextPayload>(payload) };
        if (!text) {
            return false;
        };

        text_parse = text->data;
        
        msg = text_parse.get("msg", 1);
        return true;
    }

    // Messages the proxy shows to the player. Current game clients do not print
    // "action|log" text packets, so send the OnConsoleMessage call the official
    // server uses for chat/console output instead.
    Payload write() override
    {
        GameUpdatePacket game_packet{};
        game_packet.type = PACKET_CALL_FUNCTION;
        game_packet.net_id = static_cast<std::uint32_t>(-1);
        return VariantPayload{ game_packet, PacketVariant{ std::string{ "OnConsoleMessage" }, msg } };
    }
};
}
