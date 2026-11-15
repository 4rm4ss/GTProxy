#pragma once
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "handlers/inventory_handler.hpp"
#include "handlers/map_handler.hpp"
#include "handlers/session_handler.hpp"
#include "wheel_hook.hpp"
#include "../command/command_handler.hpp"
#include "../event/event.hpp"
#include "../modules/api.hpp"
#include "../modules/host.hpp"
#include "../modules/settings.hpp"

namespace core {
// The proxy side of modules/, config/ and data/ (all next to Proxy.exe):
// finds and runs the modules, works out their settings from the active
// profile, offers them the module API (docs/modules.md), passes them the
// proxy's events and runs the console's "cfg" commands.
//
// The proxy's features (drop, worlds, looks, info, walking, reachability)
// are all modules; without modules/ the proxy only forwards packets and
// keeps track of where you are.
class ModuleSystem final : public modules::Services {
public:
    ModuleSystem(
        const std::filesystem::path& app_dir,
        event::Dispatcher& dispatcher,
        handlers::SessionHandler& session,
        handlers::InventoryHandler& inventory,
        handlers::MapHandler& map,
        command::CommandHandler& commands
    );
    ~ModuleSystem() override;

    ModuleSystem(const ModuleSystem&) = delete;
    ModuleSystem& operator=(const ModuleSystem&) = delete;

    // A line of the proxy console split in words: "cfg ..." or a module's
    // command. False if it is neither.
    bool run_console(const std::vector<std::string>& words);
    // What the console understands, one line each.
    [[nodiscard]] std::vector<std::string> console_help() const;

    [[nodiscard]] const std::string& profile() const { return profile_; }

    // Runs the modules' timers; called from the main loop.
    void update();

    // modules::Services
    [[nodiscard]] modules::WorldView world() const override;
    [[nodiscard]] modules::ReachView reach(map::TilePos tile) override;
    bool show(const map::Card& card) override;
    void reply(modules::Source source, const std::string& module_id, const std::string& text) override;

    [[nodiscard]] std::vector<modules::PlayerInfo> players() const override;
    [[nodiscard]] std::vector<modules::ObjectInfo> objects() const override;
    [[nodiscard]] std::optional<modules::TileInfo> tile(map::TilePos tile) const override;
    [[nodiscard]] modules::InventoryView inventory() const override;
    [[nodiscard]] bool items_loaded() const override;
    [[nodiscard]] std::optional<modules::ItemView> item(std::uint32_t id) const override;
    [[nodiscard]] std::vector<modules::ItemView> find_items(std::string_view query, std::size_t limit) const override;
    [[nodiscard]] char prefix() const override;

    void game_message(const std::string& text) override;
    void game_dialog(const std::string& markup) override;
    bool game_call(const modules::CallOut& call) override;
    bool server_text(const std::string& text, bool game_message) override;

    modules::WalkView walk_to(map::TilePos tile) override;
    modules::WalkView walk_stop() override;
    [[nodiscard]] bool walking() const override;
    [[nodiscard]] modules::WalkOptions walk_options() const override;
    modules::WalkOptions set_walk_options(const modules::WalkOptions& options) override;
    std::string save_setting(const std::string& module_id, const std::string& key, const modules::SettingValue& value) override;
    bool capture_wheel(const std::string& module_id, const std::string& mode) override;

private:
    void cfg(const std::vector<std::string>& words);
    void cfg_list() const;
    // Works out every setting again for 'profile' and applies it; on any
    // error keeps what is in force and says why.
    bool apply_profile(const std::string& profile, std::string_view why);
    void log_settings() const;
    // Keeps the chat commands in line with the loaded modules' commands.
    void sync_commands();

    // Packets the modules hear about.
    void listen_packets();
    template <typename Packet>
    void hook_call(const event::Event& event);
    void on_tile_change(const event::Event& event);
    void on_object(const event::Event& event);

    // gtproxy-settings.txt of earlier versions -> data/drop.json
    void import_old_settings(const std::filesystem::path& app_dir) const;

    std::filesystem::path modules_dir_;
    std::filesystem::path data_dir_;
    modules::ConfigDir config_;
    event::Dispatcher& dispatcher_;
    handlers::SessionHandler& session_;
    handlers::InventoryHandler& inventory_;
    handlers::MapHandler& map_;
    command::CommandHandler& commands_;
    command::CommandRegistry& registry_;
    modules::ModuleHost host_;
    std::vector<event::ScopedHandle> handles_;

    std::string profile_{ "default" };
    std::map<std::string, bool> enabled_;   // per module, from config/modules.json
    std::set<std::string> chat_commands_;   // added to the registry for modules

    // The map window click being answered.
    bool in_click_{ false };
    std::optional<map::Card> card_;

    // Settings changed (cfg, profile, a module's page): the "settings"
    // event goes out from the main loop.
    bool settings_changed_{ false };

    // The mouse wheel over the game, per module that asked for it.
    WheelHook wheel_;
    std::map<std::string, std::string> wheel_modes_;
    void update_wheel();
};
}
