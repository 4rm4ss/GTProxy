#pragma once
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <sol/sol.hpp>

#include "api.hpp"
#include "manifest.hpp"
#include "settings.hpp"

namespace modules {
// Events modules can listen to with gtproxy.on (docs/modules.md).
[[nodiscard]] bool known_event(std::string_view event);
// Server-to-game function calls modules can hook with gtproxy.hook_call.
[[nodiscard]] bool hookable_call(std::string_view name);

// Runs the modules that are switched on, each in its own Lua state (the
// proxy's scripting::LuaEngine with only the module API bound), so a broken
// or misbehaving module never affects the others or the scripts/ folder.
// Everything here runs on the proxy's main thread.
class ModuleHost {
public:
    struct CommandInfo {
        std::string name;
        std::string description;
        std::string module;
    };

    // A section a module adds to the /proxy menu.
    struct MenuSection {
        std::string module;
        std::string title;
        int icon{ 0 };
        std::string button;
        std::string label;
        int order{ 100 };
    };

    // What the handlers of an event decided.
    struct Outcome {
        std::size_t ran{ 0 };           // handlers that were called
        bool cancelled{ false };        // one returned "cancel"
        bool replaced{ false };         // one returned "replace" (call hooks)
    };

    // A server-to-game call as hooks see it; hooks may change the arguments.
    struct Call {
        std::string name;
        std::int32_t net_id{ -1 };
        std::vector<packet::variant> args;  // after the function name
    };

    // A Lua handler, timer or command that runs longer than this is stopped.
    static constexpr std::chrono::milliseconds TIME_LIMIT{ 2000 };

    explicit ModuleHost(Services& services);
    ~ModuleHost();

    ModuleHost(const ModuleHost&) = delete;
    ModuleHost& operator=(const ModuleHost&) = delete;

    // Where modules keep their data (gtproxy.store): <dir>/<id>.json.
    void set_data_dir(std::filesystem::path dir) { data_dir_ = std::move(dir); }

    // The installed modules (from discover()). Unloads all running ones.
    void set_available(std::vector<Manifest> manifests);
    [[nodiscard]] const std::vector<Manifest>& available() const { return available_; }
    // Default settings of every installed module, by id.
    [[nodiscard]] std::map<std::string, Settings> defaults() const;

    // Loads the modules switched on, unloads those switched off, and gives
    // every module its settings.
    void apply(const Resolved& resolved);
    // Only new settings for the loaded modules (no module is loaded or
    // unloaded): safe while a module's code is running.
    void refresh_settings(const Resolved& resolved);

    [[nodiscard]] bool loaded(std::string_view id) const;
    [[nodiscard]] std::vector<std::string> loaded_ids() const;
    [[nodiscard]] std::optional<Settings> settings(std::string_view id) const;

    // Calls the handlers of an event with a table of fields (built by
    // 'fields' in each module's own state).
    using Fields = std::function<void(sol::table& event)>;
    Outcome emit(std::string_view event, const Fields& fields = {});

    // Map window clicks (API 1 kept the right click on its own).
    std::size_t right_click(map::TilePos tile, bool in_world);

    // Lets the hooks of a server call see (and change or cancel) it.
    Outcome hook_call(Call& call);

    // Runs due timers; called from the main loop.
    void update();

    [[nodiscard]] std::vector<CommandInfo> commands() const;
    // Runs a command of a loaded module; false if no loaded module has it.
    bool run_command(std::string_view name, const std::vector<std::string>& args, Source source);

    [[nodiscard]] std::vector<MenuSection> menu_sections() const;
    // Lines the modules want on the menu's first page (their status).
    [[nodiscard]] std::vector<std::string> menu_status();

    // Names a module may not use for commands (the proxy's own commands).
    void set_reserved_names(std::function<bool(std::string_view)> reserved) { reserved_ = std::move(reserved); }
    // Called whenever modules were loaded or unloaded (their commands change).
    void set_on_change(std::function<void()> on_change) { on_change_ = std::move(on_change); }

    struct Loaded;

private:
    bool load(const Manifest& manifest, const Settings& settings);
    void unload(std::string_view id);
    [[nodiscard]] Loaded* find_command(std::string_view name) const;

    Services& services_;
    std::filesystem::path data_dir_;
    std::vector<Manifest> available_;
    std::vector<std::unique_ptr<Loaded>> loaded_;
    std::map<std::string, Settings> settings_;
    std::function<bool(std::string_view)> reserved_;
    std::function<void()> on_change_;
};
}
