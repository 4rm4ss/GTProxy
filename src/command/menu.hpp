#pragma once
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "../core/handlers/session_handler.hpp"
#include "../packet/message/dialog.hpp"

namespace command {
class CommandRegistry;

// The /proxy menu frame: a game dialog (only you see it) with where you are,
// a section per module page (modules add them) and the list of commands.
// The pages themselves belong to the modules.
class Menu {
public:
    struct Section {
        std::string title;
        int icon{ 0 };
        std::string button;     // dialog button that opens the module's page
        std::string label;
        int order{ 100 };
    };

    using Sections = std::function<std::vector<Section>()>;
    using StatusLines = std::function<std::vector<std::string>()>;
    using Runner = std::function<bool(const std::string&)>;

    Menu(core::handlers::SessionHandler& session, const CommandRegistry& registry, Runner run);

    // Where the module sections and status lines come from.
    void set_sources(Sections sections, StatusLines status);
    // The map window is on (config.json map.enabled): the menu offers it.
    void set_map_window(const bool on) { map_window_ = on; }

    void show_main() const;
    void show_commands() const;
    [[nodiscard]] std::string main_page() const;
    [[nodiscard]] std::string commands_page() const;

    // True when the dialog is the proxy's (gtproxy_*): it never reaches the
    // game server.
    [[nodiscard]] static bool owns(std::string_view dialog_name);
    // The frame's own buttons: page_main (<< Menu), commands, map.
    [[nodiscard]] static bool frame_button(std::string_view button);
    // Answers the frame's own buttons. False if the button is not one of
    // them (it is a module's).
    bool handle(const packet::message::DialogReturn& dialog) const;

private:
    core::handlers::SessionHandler& session_;
    const CommandRegistry& registry_;
    Runner run_;
    Sections sections_;
    StatusLines status_;
    bool map_window_{ false };
};
}
