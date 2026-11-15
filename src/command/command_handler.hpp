#pragma once
#include <functional>
#include <memory>
#include <string>

#include "command_registry.hpp"
#include "menu.hpp"
#include "../core/config.hpp"
#include "../core/handlers/map_handler.hpp"
#include "../core/handlers/session_handler.hpp"
#include "../core/scheduler.hpp"
#include "../event/event.hpp"
#include "../network/client.hpp"
#include "../network/server.hpp"
#include "../packet/message/dialog.hpp"

namespace command {
// Chat commands (/name) and the proxy's dialogs. The core only has /proxy,
// /proxyhelp and /map; modules and scripts add the rest.
class CommandHandler {
public:
    // frame: the button is the menu frame's own (<< Menu, All commands, Map
    // window); the frame answers it after the modules saw the dialog.
    using DialogHandler = std::function<void(const packet::message::DialogReturn&, bool frame)>;

    CommandHandler(
        core::Config& config,
        event::Dispatcher& dispatcher,
        std::shared_ptr<core::Scheduler> scheduler,
        network::Server& server,
        network::Client& client,
        core::handlers::SessionHandler& session,
        core::handlers::MapHandler& map
    );
    ~CommandHandler();

    [[nodiscard]] CommandRegistry& registry() { return registry_; }
    [[nodiscard]] const CommandRegistry& registry() const { return registry_; }
    [[nodiscard]] Menu& menu() { return menu_; }

    // Gets every proxy dialog (gtproxy_*): the modules' pages.
    void set_dialog_handler(DialogHandler handler) { dialog_handler_ = std::move(handler); }

    // Runs "name args" as if typed with the prefix; false if no such command.
    bool run(const std::string& command_line);

private:
    void register_default_commands();

    void on_text_packet(const event::Event& e);
    void on_dialog_return(const event::Event& e);

private:
    core::Config& config_;
    event::Dispatcher& dispatcher_;
    std::shared_ptr<core::Scheduler> scheduler_;
    network::Server& server_;
    network::Client& client_;
    core::handlers::SessionHandler& session_;
    core::handlers::MapHandler& map_;

    CommandRegistry registry_;
    Menu menu_;
    DialogHandler dialog_handler_;
    event::Dispatcher::Handle listener_handle_;
    event::Dispatcher::Handle dialog_listener_handle_;
};
}
