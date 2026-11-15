#pragma once
#include <string>
#include <string_view>

#include <fmt/format.h>

#include "../command.hpp"
#include "../menu.hpp"
#include "../../core/browser.hpp"
#include "../../core/handlers/map_handler.hpp"
#include "../../core/handlers/session_handler.hpp"

// The commands that stay in the core: the /proxy menu frame and the map
// window. Everything else comes from modules/.
namespace command {
class ProxyMenuCommand final : public ICommand {
public:
    explicit ProxyMenuCommand(const Menu& menu)
        : menu_{ menu }
    { }

    [[nodiscard]] std::string_view name() const override { return "proxy"; }
    [[nodiscard]] std::string description() const override { return "Open the proxy menu"; }

    Result execute(const Context&) override
    {
        menu_.show_main();
        return Result::Success;
    }

private:
    const Menu& menu_;
};

class MapCommand final : public ICommand {
public:
    MapCommand(core::handlers::SessionHandler& session, core::handlers::MapHandler& map)
        : session_{ session }
        , map_{ map }
    { }

    [[nodiscard]] std::string_view name() const override { return "map"; }
    [[nodiscard]] std::string description() const override { return "Open the map window"; }

    Result execute(const Context&) override
    {
        const auto& url{ map_.window_url() };
        if (!url) {
            session_.message("`oThe map window is off: everything is in the /proxy menu. To use it anyway, set \"map\": { \"enabled\": true } in config.json and restart the proxy.");
            return Result::Failed;
        }

        const bool opened{ core::browser::open_window(*url) };
        session_.message(fmt::format(
            "`oMap window: `w{}``{} Clicks on the map are answered by modules (walk, reachability).",
            *url,
            opened ? "" : " (open it in a browser)"
        ));
        return Result::Success;
    }

private:
    core::handlers::SessionHandler& session_;
    core::handlers::MapHandler& map_;
};
}
