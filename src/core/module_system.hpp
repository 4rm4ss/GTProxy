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
#include "handlers/player_mods.hpp"
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
        command::CommandHandler& commands,
        handlers::PlayerMods& mods
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

    modules::WalkView walk_to(map::TilePos tile, std::optional<bool> gravity) override;
    modules::WalkView walk_stop() override;
    [[nodiscard]] bool walking() const override;
    [[nodiscard]] modules::WalkOptions walk_options() const override;
    modules::WalkOptions set_walk_options(const modules::WalkOptions& options) override;
    std::string save_setting(const std::string& module_id, const std::string& key, const modules::SettingValue& value) override;
    bool capture_wheel(const std::string& module_id, const std::string& mode) override;

    [[nodiscard]] std::vector<modules::VendingInfo> vendings() const override;
    [[nodiscard]] std::vector<modules::LockInfo> locks() const override;
    [[nodiscard]] modules::WorldSummary world_summary() const override;
    [[nodiscard]] std::vector<modules::TileHit> find_tiles(std::uint16_t item) const override;
    [[nodiscard]] modules::RouteView route(map::TilePos tile, bool walk, std::optional<bool> gravity) override;
    [[nodiscard]] std::vector<int> distances() override;
    bool tile_action(map::TilePos tile, std::uint32_t item) override;
    void set_overlay(const std::string& module_id, const std::string& id, const std::optional<modules::Overlay>& overlay) override;
    void set_panel(const std::string& module_id, const std::string& id, const std::optional<std::string>& json) override;
    void request_pick(const std::string& module_id, const std::string& label) override;
    std::string render_png(std::string& error) override;
    [[nodiscard]] modules::AbilitiesView abilities() const override;
    modules::AbilitiesView set_abilities(const modules::Abilities& abilities) override;
    [[nodiscard]] bool key_down(std::string_view key) const override;
    [[nodiscard]] bool map_window() const override;
    bool send_raw(bool to_game, std::span<const std::byte> data) override;
    void set_map_window(const bool on) { map_window_ = on; }

    // The game server of config.json: modules keep data per server and world.
    void set_server_name(std::string name) { server_name_ = std::move(name); }

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

    // The modules' part of the map window.
    void publish_module_view();
    void forget_module_view(bool everything);
    [[nodiscard]] std::string answer_panel(const std::map<std::string, std::string>& params);
    // Server texts the game shows (OnConsoleMessage, OnTalkBubble ...).
    void on_raw_client_packet(const event::Event& event);
    // Every packet, both ways, offered to the modules that watch them.
    void on_raw_packet(const event::Event& event, bool from_server);

    std::filesystem::path modules_dir_;
    std::filesystem::path data_dir_;
    modules::ConfigDir config_;
    event::Dispatcher& dispatcher_;
    handlers::SessionHandler& session_;
    handlers::InventoryHandler& inventory_;
    handlers::MapHandler& map_;
    command::CommandHandler& commands_;
    command::CommandRegistry& registry_;
    handlers::PlayerMods& mods_;
    bool map_window_{ false };
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

    std::string server_name_;
    std::map<std::string, modules::Overlay> overlays_;     // "<module>/<id>"
    std::map<std::string, std::string> panels_;            // "<module>/<id>" -> JSON
    std::string pick_owner_;

    // What a packet changes, seen before the world handler applies it.
    struct TileBefore {
        int x{ 0 };
        int y{ 0 };
        std::uint16_t fg{ 0 };
        std::uint16_t bg{ 0 };
    };
    std::optional<TileBefore> tile_before_;
    struct ObjectBefore {
        std::uint32_t id{ 0 };
        std::uint16_t item{ 0 };
        std::uint8_t amount{ 0 };
        float x{ 0.0f };
        float y{ 0.0f };
    };
    std::optional<ObjectBefore> object_before_;
    struct PlayerBefore {
        std::int32_t net_id{ -1 };
        std::int32_t user_id{ 0 };
        std::string name;
    };
    std::optional<PlayerBefore> player_before_;

    // The mouse wheel over the game, per module that asked for it.
    WheelHook wheel_;
    std::map<std::string, std::string> wheel_modes_;
    void update_wheel();
};
}
