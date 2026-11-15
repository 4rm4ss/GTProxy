#include "core.hpp"

#include <chrono>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <enet/enet.h>
#include <spdlog/spdlog.h>

#include "browser.hpp"
#include "paths.hpp"
#include "../packet/register_packets.hpp"
#include "../scripting/bindings/command_bindings.hpp"
#include "../scripting/bindings/event_bindings.hpp"
#include "../scripting/bindings/logger_bindings.hpp"
#include "../scripting/bindings/packet_bindings.hpp"
#include "../scripting/bindings/scheduler_bindings.hpp"
#include "../scripting/bindings/player_bindings.hpp"
#include "../scripting/bindings/world_bindings.hpp"
#include "../scripting/bindings/world_data_bindings.hpp"
#include "../scripting/bindings/item_bindings.hpp"

namespace core {
Core::Core()
    : running_{ true }
    , scheduler_{ std::make_shared<Scheduler>() }
{
    spdlog::info(
        "Config: server {} | proxy port {} | DNS {} | prefix '{}'",
        config_.get_server_config().address,
        config_.get_server_config().port,
        config_.get_client_config().dns_server,
        config_.get_command_config().prefix
    );

    if (enet_initialize() != 0) {
        throw std::runtime_error{ "Failed to initialize ENet" };
    }

    server_ = std::make_unique<network::Server>(config_, dispatcher_);
    client_ = std::make_unique<network::Client>(config_, dispatcher_);
    web_server_ = std::make_unique<WebServer>(config_, dispatcher_, *client_, *server_);
    map_server_ = std::make_unique<MapServer>(config_.get_map_config(), map_bridge_);

    packet::register_all_packets();

    connection_handler_ = std::make_unique<handlers::ConnectionHandler>(dispatcher_, *client_, *server_, config_);
    forwarding_handler_ = std::make_unique<handlers::ForwardingHandler>(dispatcher_, *client_, *server_);
    world_handler_ = std::make_unique<handlers::WorldHandler>(dispatcher_);
    session_handler_ = std::make_unique<handlers::SessionHandler>(dispatcher_, *server_, *client_);
    inventory_handler_ = std::make_unique<handlers::InventoryHandler>(dispatcher_, *session_handler_);
    map_handler_ = std::make_unique<handlers::MapHandler>(
        dispatcher_,
        *server_,
        *client_,
        *session_handler_,
        config_.get_map_config(),
        map_bridge_,
        map_server_->url()
    );
    player_mods_ = std::make_unique<handlers::PlayerMods>(dispatcher_, *client_, *server_, *session_handler_);
    command_handler_ = std::make_unique<command::CommandHandler>(
        config_,
        dispatcher_,
        scheduler_,
        *server_,
        *client_,
        *session_handler_,
        *map_handler_
    );

    script_engine_ = std::make_unique<scripting::LuaEngine>();

    script_scheduler_ = std::make_unique<scripting::ScriptScheduler>(*script_engine_);

    script_event_bridge_ = std::make_unique<scripting::ScriptEventBridge>(
        dispatcher_,
        *script_engine_,
        *client_,
        *server_
    );

    script_engine_->register_binding(std::make_unique<scripting::bindings::CommandBindings>(*command_handler_, *server_, *client_, dispatcher_, scheduler_));
    script_engine_->register_binding(std::make_unique<scripting::bindings::EventBindings>(*script_event_bridge_));
    script_engine_->register_binding(std::make_unique<scripting::bindings::LoggerBindings>());
    script_engine_->register_binding(std::make_unique<scripting::bindings::PacketBindings>(*client_, *server_));
    script_engine_->register_binding(std::make_unique<scripting::bindings::SchedulerBindings>(*script_scheduler_));
    script_engine_->register_binding(std::make_unique<scripting::bindings::PlayerBindings>());
    script_engine_->register_binding(std::make_unique<scripting::bindings::WorldBindings>());
    script_engine_->register_binding(std::make_unique<scripting::bindings::WorldDataBindings>());
    script_engine_->register_binding(std::make_unique<scripting::bindings::ItemBindings>());

    script_loader_ = std::make_unique<scripting::ScriptLoader>(*script_engine_, "scripts");
    script_loader_->load_all();

    spdlog::info("Core initialized successfully");

    if (const auto url{ map_server_->url() }; url && config_.get_map_config().open_on_start) {
        browser::open_window(*url);
    }

    // modules/, config/ and data/ are next to Proxy.exe, whatever folder it
    // is started from. The proxy's features are all modules.
    module_system_ = std::make_unique<ModuleSystem>(
        paths::executable_dir(),
        dispatcher_,
        *session_handler_,
        *inventory_handler_,
        *map_handler_,
        *command_handler_,
        *player_mods_
    );
    module_system_->set_server_name(config_.get_server_config().address);
    module_system_->set_map_window(map_server_->url().has_value());
    command_handler_->menu().set_map_window(map_server_->url().has_value());

    console_ = std::make_unique<ConsoleInput>();
    spdlog::info("Proxy console: type 'help' here for its commands (cfg list / cfg exec <profile> / cfg reload ...)");
}

void Core::run_console_line(const std::string& line) const
{
    std::istringstream words_in{ line };
    std::vector<std::string> words;
    for (std::string word; words_in >> word;) {
        words.push_back(std::move(word));
    }
    if (words.empty() || module_system_->run_console(words)) {
        return;
    }

    if (words[0] != "help") {
        spdlog::warn("Unknown console command '{}'", words[0]);
    }
    spdlog::info("Proxy console commands:");
    for (const auto& help : module_system_->console_help()) {
        spdlog::info("  {}", help);
    }
}

Core::~Core()
{
    // Tear down in dependency order instead of reverse declaration order:
    // stop background work first, release everything that holds Lua
    // references (registered script commands, callbacks, scheduled tasks)
    // before the Lua state is closed, and destroy ENet hosts before ENet is
    // deinitialized.
    if (scheduler_) {
        scheduler_->stop();
    }

    // Modules hold commands in the command registry and hooks in the
    // handlers: they go first.
    console_.reset();
    module_system_.reset();

    script_loader_.reset();
    script_event_bridge_.reset();
    script_scheduler_.reset();
    command_handler_.reset();
    script_engine_.reset();
    player_mods_.reset();
    map_handler_.reset();
    inventory_handler_.reset();
    session_handler_.reset();

    world_handler_.reset();
    forwarding_handler_.reset();
    connection_handler_.reset();
    map_server_.reset();
    web_server_.reset();
    client_.reset();
    server_.reset();

    enet_deinitialize();
}

void Core::run() const
{
    constexpr auto sleep_timer = std::chrono::microseconds{ 5000 };
    auto prev = std::chrono::high_resolution_clock::now();
    auto sleep_duration = sleep_timer;

    while (running_) {
        const auto now = std::chrono::high_resolution_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - prev);
        prev = now;

        sleep_duration += sleep_timer - elapsed;

        server_->process();
        client_->process();
        client_->check_connect_progress();
        map_handler_->update();
        module_system_->update();
        for (const auto& line : console_->take_lines()) {
            run_console_line(line);
        }
        script_scheduler_->update(elapsed);

        if (sleep_duration > std::chrono::microseconds::zero()) {
            std::this_thread::sleep_for(sleep_duration);
        }

        if (sleep_duration >= sleep_timer) {
            sleep_duration = std::chrono::microseconds::zero();
        }
    }
}
}
