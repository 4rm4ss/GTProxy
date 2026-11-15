#pragma once
#include <atomic>

#include "config.hpp"
#include "console_input.hpp"
#include "module_system.hpp"
#include "map_server.hpp"
#include "scheduler.hpp"
#include "web_server.hpp"
#include "handlers/connection_handler.hpp"
#include "handlers/feature_handler.hpp"
#include "handlers/inventory_handler.hpp"
#include "handlers/map_handler.hpp"
#include "handlers/forwarding_handler.hpp"
#include "handlers/world_handler.hpp"
#include "../command/command_handler.hpp"
#include "../map/bridge.hpp"
#include "../network/client.hpp"
#include "../network/server.hpp"
#include "../scripting/lua_engine.hpp"
#include "../scripting/script_event_bridge.hpp"
#include "../scripting/script_loader.hpp"
#include "../scripting/script_scheduler.hpp"
#include "../utils/types.hpp"

namespace core {
class Core final : public utils::types::Immobile {
public:
    Core();
    ~Core();

    void run() const;
    // Safe to call from a signal handler.
    void stop() { running_.store(false); }

    [[nodiscard]] Config& get_config() { return config_; }

private:
    // A command typed into the proxy's console window.
    void run_console_line(const std::string& line) const;

    Config config_;
    std::atomic<bool> running_;

    event::Dispatcher dispatcher_;
    std::shared_ptr<Scheduler> scheduler_;

    std::unique_ptr<network::Server> server_;
    std::unique_ptr<network::Client> client_;
    std::unique_ptr<WebServer> web_server_;
    // Declared before map_server_: its HTTP threads use the bridge until the
    // server is gone.
    map::Bridge map_bridge_;
    std::unique_ptr<MapServer> map_server_;

    std::unique_ptr<handlers::ConnectionHandler> connection_handler_;
    std::unique_ptr<handlers::ForwardingHandler> forwarding_handler_;
    std::unique_ptr<handlers::WorldHandler> world_handler_;
    std::unique_ptr<handlers::FeatureHandler> feature_handler_;
    std::unique_ptr<handlers::InventoryHandler> inventory_handler_;
    std::unique_ptr<handlers::MapHandler> map_handler_;
    std::unique_ptr<command::CommandHandler> command_handler_;

    std::unique_ptr<scripting::LuaEngine> script_engine_;
    std::unique_ptr<scripting::ScriptScheduler> script_scheduler_;
    std::unique_ptr<scripting::ScriptEventBridge> script_event_bridge_;
    std::unique_ptr<scripting::ScriptLoader> script_loader_;

    std::unique_ptr<ModuleSystem> module_system_;
    std::unique_ptr<ConsoleInput> console_;
};
}
