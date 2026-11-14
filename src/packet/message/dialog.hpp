#pragma once
#include <string>

#include "../packet_helper.hpp"

namespace packet::message {
// What the game sends when a dialog (OnDialogRequest) button is pressed:
//   action|dialog_return
//   dialog_name|<name from end_dialog>
//   <input name>|<value>        one line per text input / checkbox
//   buttonClicked|<button name> (missing when the dialog's OK button is used)
struct DialogReturn : TextPacket<PacketId::DialogReturn, NET_MESSAGE_GENERIC_TEXT> {
    std::string dialog_name;
    std::string button_clicked;

    bool read(const Payload& payload) override
    {
        const auto text{ get_payload_if<TextPayload>(payload) };
        if (!text) {
            return false;
        }

        text_parse = text->data;
        dialog_name = text_parse.get("dialog_name", 0);
        button_clicked = text_parse.get("buttonClicked", 0);
        return true;
    }

    // Value of a text input or checkbox. Values the player typed may contain
    // '|', which the text format splits on, so the parts are joined back.
    [[nodiscard]] std::string field(const std::string& name) const
    {
        for (const auto& [key, values] : text_parse.get_data()) {
            if (key != name) {
                continue;
            }

            std::string value;
            for (std::size_t i{ 0 }; i < values.size(); ++i) {
                if (i > 0) {
                    value += '|';
                }
                value += values[i];
            }
            return value;
        }

        return {};
    }

    [[nodiscard]] bool checked(const std::string& name) const
    {
        return field(name) == "1";
    }

    Payload write() override
    {
        return TextPayload{ MESSAGE_TYPE, text_parse };
    }
};
}
