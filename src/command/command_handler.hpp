#pragma once
#include "command_registry.hpp"
#include "../core/config.hpp"
#include "../core/handlers/feature_handler.hpp"
#include "../core/handlers/inventory_handler.hpp"
#include "menu.hpp"
#include "../core/scheduler.hpp"
#include "../event/event.hpp"
#include "../network/client.hpp"
#include "../network/server.hpp"

namespace command {
class CommandHandler {
public:
    CommandHandler(
        core::Config& config,
        event::Dispatcher& dispatcher,
        std::shared_ptr<core::Scheduler> scheduler,
        network::Server& server,
        network::Client& client,
        core::handlers::FeatureHandler& features,
        core::handlers::InventoryHandler& inventory
    );
    ~CommandHandler();

    [[nodiscard]] CommandRegistry& registry() { return registry_; }
    [[nodiscard]] const CommandRegistry& registry() const { return registry_; }

private:
    void register_default_commands();

    void on_text_packet(const event::Event& e);
    void on_dialog_return(const event::Event& e);
    bool run(const std::string& command_line);

private:
    core::Config& config_;
    event::Dispatcher& dispatcher_;
    std::shared_ptr<core::Scheduler> scheduler_;
    network::Server& server_;
    network::Client& client_;
    core::handlers::FeatureHandler& features_;
    core::handlers::InventoryHandler& inventory_;
    Menu menu_;

    CommandRegistry registry_;
    event::Dispatcher::Handle listener_handle_;
    event::Dispatcher::Handle dialog_listener_handle_;
};
}
