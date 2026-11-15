#pragma once
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "handlers/feature_handler.hpp"
#include "handlers/map_handler.hpp"
#include "../command/command_registry.hpp"
#include "../modules/api.hpp"
#include "../modules/host.hpp"
#include "../modules/settings.hpp"

namespace core {
// The proxy side of modules/ and config/ (both next to Proxy.exe):
// finds and runs the modules, works out their settings from the active
// profile, offers them the module API (map clicks, world, reachability,
// map window, log) and runs the console's "cfg" commands.
class ModuleSystem final : public modules::Services {
public:
    ModuleSystem(
        const std::filesystem::path& app_dir,
        handlers::MapHandler& map,
        handlers::FeatureHandler& features,
        command::CommandRegistry& registry
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

    // modules::Services
    [[nodiscard]] modules::WorldView world() const override;
    [[nodiscard]] modules::ReachView reach(map::TilePos tile) override;
    bool show(const map::Card& card) override;
    void reply(modules::Source source, const std::string& module_id, const std::string& text) override;

private:
    void cfg(const std::vector<std::string>& words);
    void cfg_list() const;
    // Works out every setting again for 'profile' and applies it; on any
    // error keeps what is in force and says why.
    bool apply_profile(const std::string& profile, std::string_view why);
    void log_settings() const;
    // Keeps the chat commands in line with the loaded modules' commands.
    void sync_commands();

    std::filesystem::path modules_dir_;
    modules::ConfigDir config_;
    handlers::MapHandler& map_;
    handlers::FeatureHandler& features_;
    command::CommandRegistry& registry_;
    modules::ModuleHost host_;

    std::string profile_{ "default" };
    std::map<std::string, bool> enabled_;   // per module, from config/modules.json
    std::set<std::string> chat_commands_;   // added to the registry for modules

    // The map window click being answered.
    bool in_click_{ false };
    std::optional<map::Card> card_;
};
}
