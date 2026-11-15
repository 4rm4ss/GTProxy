#include "menu.hpp"

#include <algorithm>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "command_registry.hpp"
#include "../core/features.hpp"

namespace command {
namespace {
using core::features::sanitize;

constexpr int ICON_MENU{ 32 };      // Wrench
constexpr std::string_view PREFIX{ "gtproxy_" };

std::string header(const std::string& title)
{
    return fmt::format("set_default_color|`o\nadd_label_with_icon|big|`w{}``|left|{}|\n", title, ICON_MENU);
}

std::string button(const std::string& name, const std::string& label)
{
    return fmt::format("add_button|{}|{}|noflags|0|0|\n", name, label);
}

std::string end(const std::string& name)
{
    return fmt::format("add_quick_exit|\nend_dialog|{}|Close||\n", name);
}
}

Menu::Menu(core::handlers::SessionHandler& session, const CommandRegistry& registry, Runner run)
    : session_{ session }
    , registry_{ registry }
    , run_{ std::move(run) }
{ }

void Menu::set_sources(Sections sections, StatusLines status)
{
    sections_ = std::move(sections);
    status_ = std::move(status);
}

bool Menu::owns(const std::string_view dialog_name)
{
    return dialog_name.starts_with(PREFIX);
}

void Menu::show_main() const
{
    session_.show_dialog(main_page());
}

void Menu::show_commands() const
{
    session_.show_dialog(commands_page());
}

std::string Menu::main_page() const
{
    std::string text{ header("GTProxy") };

    if (session_.in_world()) {
        std::string where{ fmt::format("`oWorld: `w{}``", sanitize(session_.current_world())) };
        if (const auto pos{ session_.local_position() }) {
            where += fmt::format("   Tile: `w{}, {}``", static_cast<int>(pos->x / 32.0f), static_cast<int>(pos->y / 32.0f));
        }
        text += fmt::format("add_smalltext|{}|\n", where);
    }
    else {
        text += "add_smalltext|`oNot in a world|\n";
    }
    if (status_) {
        for (const auto& line : status_()) {
            text += fmt::format("add_smalltext|{}|\n", sanitize(line));
        }
    }

    auto sections{ sections_ ? sections_() : std::vector<Section>{} };
    std::ranges::stable_sort(sections, {}, &Section::order);
    text += "add_spacer|small|\n";
    if (sections.empty()) {
        text += "add_smalltext|`oNo modules: put them in modules/ next to Proxy.exe.|\n";
    }
    for (const auto& section : sections) {
        text += fmt::format("add_label_with_icon|small|`w{}``|left|{}|\n", sanitize(section.title), section.icon);
        text += button(sanitize(section.button), sanitize(section.label));
    }

    text += "add_spacer|small|\n";
    text += button("map", "Map window (click a tile to walk there)");
    text += button("commands", "All proxy commands");
    return text + end("gtproxy_menu");
}

std::string Menu::commands_page() const
{
    std::string text{ header("Commands") };
    text += fmt::format("add_smalltext|`oType them in chat. They never reach the server. Prefix: `w{}``|\n", registry_.prefix());
    text += "add_spacer|small|\n";

    auto commands{ registry_.get_all_commands() };
    std::ranges::sort(commands);
    for (const auto& [name, description] : commands) {
        text += fmt::format("add_smalltext|`w{}{}`` `o- {}|\n", registry_.prefix(), sanitize(name), sanitize(description));
    }
    text += "add_spacer|small|\n";
    text += button("page_main", "<< Menu");
    return text + end("gtproxy_commands");
}

bool Menu::frame_button(const std::string_view button)
{
    return button == "page_main" || button == "commands" || button == "map";
}

bool Menu::handle(const packet::message::DialogReturn& dialog) const
{
    const std::string& name{ dialog.button_clicked };
    spdlog::info("Proxy menu: {} {}", dialog.dialog_name, name.empty() ? "apply" : name);

    if (name == "page_main") {
        show_main();
        return true;
    }
    if (name == "commands") {
        show_commands();
        return true;
    }
    if (name == "map") {
        run_("map");
        return true;
    }
    return false;
}
}
