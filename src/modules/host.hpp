#pragma once
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "api.hpp"
#include "manifest.hpp"
#include "settings.hpp"

namespace modules {
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

    // A Lua handler that runs longer than this is stopped.
    static constexpr std::chrono::milliseconds TIME_LIMIT{ 2000 };

    explicit ModuleHost(Services& services);
    ~ModuleHost();

    ModuleHost(const ModuleHost&) = delete;
    ModuleHost& operator=(const ModuleHost&) = delete;

    // The installed modules (from discover()). Unloads all running ones.
    void set_available(std::vector<Manifest> manifests);
    [[nodiscard]] const std::vector<Manifest>& available() const { return available_; }
    // Default settings of every installed module, by id.
    [[nodiscard]] std::map<std::string, Settings> defaults() const;

    // Loads the modules switched on, unloads those switched off, and gives
    // every module its settings.
    void apply(const Resolved& resolved);

    [[nodiscard]] bool loaded(std::string_view id) const;
    [[nodiscard]] std::vector<std::string> loaded_ids() const;
    [[nodiscard]] std::optional<Settings> settings(std::string_view id) const;

    // Map window right click on a tile. Returns how many handlers ran.
    std::size_t right_click(map::TilePos tile, bool in_world);

    [[nodiscard]] std::vector<CommandInfo> commands() const;
    // Runs a command of a loaded module; false if no loaded module has it.
    bool run_command(std::string_view name, const std::vector<std::string>& args, Source source);

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
    std::vector<Manifest> available_;
    std::vector<std::unique_ptr<Loaded>> loaded_;
    std::map<std::string, Settings> settings_;
    std::function<bool(std::string_view)> reserved_;
    std::function<void()> on_change_;
};
}
