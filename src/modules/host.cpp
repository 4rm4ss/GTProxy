#include "host.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

#include <fmt/format.h>
#include <sol/sol.hpp>
#include <spdlog/spdlog.h>

#include "json.hpp"
#include "../scripting/binding_module.hpp"
#include "../scripting/lua_engine.hpp"

namespace modules {
struct ModuleHost::Loaded {
    struct Command {
        std::string description;
        sol::protected_function run;
    };

    Manifest manifest;
    Settings settings;
    // Declared before everything holding Lua references: destroyed last.
    std::unique_ptr<scripting::LuaEngine> engine;
    std::vector<sol::protected_function> right_click;
    std::map<std::string, Command> commands;
};

namespace {
// Stops a module whose Lua code runs too long (an endless loop would
// otherwise freeze the whole proxy).
thread_local std::chrono::steady_clock::time_point g_deadline{};

void watchdog_hook(lua_State* lua, lua_Debug*)
{
    if (std::chrono::steady_clock::now() > g_deadline) {
        luaL_error(lua, "stopped: it ran longer than %d ms", static_cast<int>(ModuleHost::TIME_LIMIT.count()));
    }
}

class Watchdog {
public:
    explicit Watchdog(lua_State* lua)
        : lua_{ lua }
    {
        g_deadline = std::chrono::steady_clock::now() + ModuleHost::TIME_LIMIT;
        lua_sethook(lua_, watchdog_hook, LUA_MASKCOUNT, 10'000);
    }
    ~Watchdog() { lua_sethook(lua_, nullptr, 0, 0); }

    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;

private:
    lua_State* lua_;
};

std::string as_text(const sol::object& value)
{
    if (value.is<std::string>()) {
        return value.as<std::string>();
    }
    sol::state_view lua{ value.lua_state() };
    const sol::protected_function to_string{ lua["tostring"] };
    const sol::protected_function_result text{ to_string(value) };
    return text.valid() ? text.get<std::string>() : std::string{ "?" };
}

int whole_number(const double value, const char* what)
{
    if (!std::isfinite(value) || value != std::floor(value)
        || value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
        throw std::runtime_error{ fmt::format("{} must be a whole number", what) };
    }
    return static_cast<int>(value);
}

bool valid_command_name(const std::string_view name)
{
    if (name.empty() || name.size() > 32 || name[0] < 'a' || name[0] > 'z') {
        return false;
    }
    return std::ranges::all_of(name, [](const char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

// The `gtproxy` table: everything a module can use (API version 1).
class ModuleApi final : public scripting::IBindingModule {
public:
    ModuleApi(ModuleHost::Loaded& module, Services& services, std::function<bool(std::string_view)> name_taken)
        : module_{ module }
        , services_{ services }
        , name_taken_{ std::move(name_taken) }
    { }

    [[nodiscard]] std::string_view name() const override { return "gtproxy module API"; }

    // Every function handed to Lua captures plain pointers only: sol2 frees
    // captured objects with destructors (std::string ...) at the wrong place
    // when the Lua state closes.
    void bind(sol::state& lua) override
    {
        auto* const module{ &module_ };
        auto* const services{ &services_ };
        auto* const self{ this };

        // Modules get the module API and the safe standard libraries only.
        lua["dofile"] = sol::lua_nil;
        lua["loadfile"] = sol::lua_nil;
        lua.set_function("print", [module](const sol::variadic_args& args) {
            std::string line;
            for (const auto& arg : args) {
                line += (line.empty() ? "" : " ") + as_text(arg);
            }
            spdlog::info("[{}] {}", module->manifest.id, line);
        });

        sol::table api{ lua.create_named_table("gtproxy") };
        api["api_version"] = API_VERSION;
        api["module"] = lua.create_table_with(
            "id", module->manifest.id,
            "version", module->manifest.version,
            "name", module->manifest.name
        );

        sol::table log{ lua.create_table() };
        log.set_function("info", [module](const sol::object& text) { spdlog::info("[{}] {}", module->manifest.id, as_text(text)); });
        log.set_function("warn", [module](const sol::object& text) { spdlog::warn("[{}] {}", module->manifest.id, as_text(text)); });
        log.set_function("error", [module](const sol::object& text) { spdlog::error("[{}] {}", module->manifest.id, as_text(text)); });
        api["log"] = log;

        api.set_function("on", [module](const std::string& event, const sol::object& handler) {
            if (event != "right_click") {
                throw std::runtime_error{ fmt::format("gtproxy.on: unknown event '{}' (API {} has: right_click)", event, API_VERSION) };
            }
            if (handler.get_type() != sol::type::function) {
                throw std::runtime_error{ "gtproxy.on: the handler must be a function" };
            }
            module->right_click.push_back(handler.as<sol::protected_function>());
        });

        api.set_function("command", [module, self](const std::string& name, const std::string& description, const sol::object& run) {
            if (!valid_command_name(name)) {
                throw std::runtime_error{ fmt::format("gtproxy.command: '{}' is not a valid command name (a-z, 0-9, '_')", name) };
            }
            if (run.get_type() != sol::type::function) {
                throw std::runtime_error{ "gtproxy.command: the command must be a function" };
            }
            if (module->commands.contains(name) || self->name_taken_(name)) {
                spdlog::error("[{}] gtproxy.command: '{}' is already a command of the proxy or another module; not added", module->manifest.id, name);
                return false;
            }
            module->commands[name] = ModuleHost::Loaded::Command{ description, run.as<sol::protected_function>() };
            return true;
        });

        api.set_function("setting", [module](sol::this_state state, const std::string& key) {
            const auto it{ module->settings.find(key) };
            if (it == module->settings.end()) {
                throw std::runtime_error{ fmt::format("gtproxy.setting: '{}' is not a setting of this module (module.json \"settings\")", key) };
            }
            return std::visit([&](const auto& value) { return sol::make_object(state, value); }, it->second);
        });

        sol::table world{ lua.create_table() };
        world.set_function("info", [services](sol::this_state state) {
            sol::state_view view_lua{ state };
            const auto view{ services->world() };
            sol::table info{ view_lua.create_table() };
            info["in_world"] = view.in_world;
            info["name"] = view.name;
            info["width"] = view.width;
            info["height"] = view.height;
            if (view.me) {
                info["me"] = view_lua.create_table_with(
                    "x", view.me->x,
                    "y", view.me->y,
                    "tile_x", view.me->tile_x,
                    "tile_y", view.me->tile_y
                );
            }
            return info;
        });
        api["world"] = world;

        api.set_function("reach", [services](sol::this_state state, const double x, const double y) {
            sol::state_view view_lua{ state };
            const auto result{ services->reach({ whole_number(x, "gtproxy.reach: x"), whole_number(y, "gtproxy.reach: y") }) };
            sol::table table{ view_lua.create_table() };
            table["x"] = result.x;
            table["y"] = result.y;
            table["status"] = result.status;
            table["headline"] = result.headline;
            table["reason"] = result.reason;
            table["message"] = result.message;
            table["missing"] = result.missing;
            table["steps"] = result.steps;
            table["steps_word"] = result.steps_word;
            table["tile_text"] = result.tile_text;
            table["summary"] = result.summary;
            if (result.start) {
                table["start_x"] = result.start->x;
                table["start_y"] = result.start->y;
            }
            return table;
        });

        api.set_function("show", [services](const sol::object& value) {
            if (value.get_type() != sol::type::table) {
                throw std::runtime_error{ "gtproxy.show: expects a table { title = ..., text = ..., kind = ..., marker = ... }" };
            }
            const auto options{ value.as<sol::table>() };
            map::Card card{};
            card.title = options.get_or<std::string>("title", "");
            card.text = options.get_or<std::string>("text", "");
            card.kind = options.get_or<std::string>("kind", "none");
            card.status = options.get_or<std::string>("status", "");
            card.marker = options.get_or("marker", true);
            if (card.kind != "ok" && card.kind != "bad" && card.kind != "none") {
                throw std::runtime_error{ fmt::format("gtproxy.show: kind '{}' must be \"ok\", \"bad\" or \"none\"", card.kind) };
            }
            const sol::object x{ options["x"] };
            const sol::object y{ options["y"] };
            if (x.get_type() == sol::type::number && y.get_type() == sol::type::number) {
                card.tile = map::TilePos{ whole_number(x.as<double>(), "gtproxy.show: x"), whole_number(y.as<double>(), "gtproxy.show: y") };
            }
            return services->show(card);
        });
    }

private:
    ModuleHost::Loaded& module_;
    Services& services_;
    std::function<bool(std::string_view)> name_taken_;
};
}

ModuleHost::ModuleHost(Services& services)
    : services_{ services }
{ }

ModuleHost::~ModuleHost()
{
    // Lua references first, then the states (Loaded's member order).
    loaded_.clear();
}

void ModuleHost::set_available(std::vector<Manifest> manifests)
{
    const bool had_modules{ !loaded_.empty() };
    loaded_.clear();
    available_ = std::move(manifests);
    if (had_modules && on_change_) {
        on_change_();
    }
}

std::map<std::string, Settings> ModuleHost::defaults() const
{
    std::map<std::string, Settings> defaults;
    for (const auto& manifest : available_) {
        defaults.emplace(manifest.id, manifest.settings);
    }
    return defaults;
}

void ModuleHost::apply(const Resolved& resolved)
{
    settings_ = resolved.settings;
    const auto enabled = [&](const std::string& id) {
        const auto it{ resolved.enabled.find(id) };
        return it == resolved.enabled.end() || it->second;
    };

    bool changed{ false };
    for (const auto& id : loaded_ids()) {
        if (!enabled(id)) {
            unload(id);
            spdlog::info("Module '{}' unloaded: switched off in config/modules.json", id);
            changed = true;
        }
    }

    for (auto& module : loaded_) {
        if (const auto it{ settings_.find(module->manifest.id) }; it != settings_.end()) {
            module->settings = it->second;
        }
    }

    for (const auto& manifest : available_) {
        if (!enabled(manifest.id)) {
            if (!loaded(manifest.id)) {
                spdlog::info("Module '{}' {} is switched off in config/modules.json", manifest.id, manifest.version);
            }
            continue;
        }
        if (!loaded(manifest.id)) {
            const auto it{ settings_.find(manifest.id) };
            changed = load(manifest, it != settings_.end() ? it->second : manifest.settings) || changed;
        }
    }

    if (changed && on_change_) {
        on_change_();
    }
}

bool ModuleHost::load(const Manifest& manifest, const Settings& settings)
{
    auto module{ std::make_unique<Loaded>() };
    module->manifest = manifest;
    module->settings = settings;
    module->engine = std::make_unique<scripting::LuaEngine>();
    module->engine->register_binding(std::make_unique<ModuleApi>(*module, services_, [this](const std::string_view name) {
        return (reserved_ && reserved_(name)) || find_command(name) != nullptr;
    }));

    const auto code{ json::read_text(manifest.entry()) };
    bool ok{ code.has_value() };
    if (!code) {
        spdlog::error("Module '{}': cannot read {}", manifest.id, manifest.main);
    }
    else {
        const Watchdog watchdog{ module->engine->state().lua_state() };
        ok = module->engine->execute(*code, fmt::format("@modules/{}/{}", manifest.dir.filename().generic_string(), manifest.main));
    }

    if (!ok) {
        spdlog::error("Module '{}' {} failed to load (see above); the other modules keep working", manifest.id, manifest.version);
        return false;
    }

    std::string commands;
    for (const auto& [name, command] : module->commands) {
        commands += (commands.empty() ? "" : ", ") + name;
    }
    spdlog::info(
        "Module '{}' {} loaded from {}: {} right click handler(s), commands: {}",
        manifest.id,
        manifest.version,
        "modules/" + manifest.dir.filename().generic_string(),
        module->right_click.size(),
        commands.empty() ? std::string{ "none" } : commands
    );
    loaded_.push_back(std::move(module));
    return true;
}

void ModuleHost::unload(const std::string_view id)
{
    std::erase_if(loaded_, [&](const std::unique_ptr<Loaded>& module) { return module->manifest.id == id; });
}

bool ModuleHost::loaded(const std::string_view id) const
{
    return std::ranges::any_of(loaded_, [&](const std::unique_ptr<Loaded>& module) { return module->manifest.id == id; });
}

std::vector<std::string> ModuleHost::loaded_ids() const
{
    std::vector<std::string> ids;
    for (const auto& module : loaded_) {
        ids.push_back(module->manifest.id);
    }
    return ids;
}

std::optional<Settings> ModuleHost::settings(const std::string_view id) const
{
    if (const auto it{ settings_.find(std::string{ id }) }; it != settings_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::size_t ModuleHost::right_click(const map::TilePos tile, const bool in_world)
{
    std::size_t ran{ 0 };
    for (const auto& module : loaded_) {
        for (const auto& handler : module->right_click) {
            sol::state& lua{ module->engine->state() };
            const sol::table event{ lua.create_table_with("x", tile.x, "y", tile.y, "in_world", in_world) };

            const Watchdog watchdog{ lua.lua_state() };
            const sol::protected_function_result result{ handler(event) };
            ++ran;
            if (!result.valid()) {
                const sol::error error{ result };
                spdlog::error("[{}] right_click handler failed: {}", module->manifest.id, error.what());
            }
        }
    }
    return ran;
}

std::vector<ModuleHost::CommandInfo> ModuleHost::commands() const
{
    std::vector<CommandInfo> commands;
    for (const auto& module : loaded_) {
        for (const auto& [name, command] : module->commands) {
            commands.push_back({ name, command.description, module->manifest.id });
        }
    }
    return commands;
}

ModuleHost::Loaded* ModuleHost::find_command(const std::string_view name) const
{
    for (const auto& module : loaded_) {
        if (module->commands.contains(std::string{ name })) {
            return module.get();
        }
    }
    return nullptr;
}

bool ModuleHost::run_command(const std::string_view name, const std::vector<std::string>& args, const Source source)
{
    auto* module{ find_command(name) };
    if (!module) {
        return false;
    }

    sol::state& lua{ module->engine->state() };
    sol::table arguments{ lua.create_table() };
    for (std::size_t i{ 0 }; i < args.size(); ++i) {
        arguments[i + 1] = args[i];
    }

    const std::string id{ module->manifest.id };
    sol::table context{ lua.create_table() };
    context["source"] = source == Source::Game ? "game" : "console";
    context["name"] = std::string{ name };
    // ctx.reply(text) and ctx:reply(text) both work. Plain pointers only
    // in the capture (see ModuleApi::bind).
    auto* const services{ &services_ };
    context.set_function("reply", [services, source, module](const sol::variadic_args& args) {
        for (const auto& arg : args) {
            if (arg.get_type() != sol::type::table) {
                services->reply(source, module->manifest.id, as_text(arg));
                return;
            }
        }
    });

    const Watchdog watchdog{ lua.lua_state() };
    const sol::protected_function_result result{ module->commands.at(std::string{ name }).run(arguments, context) };
    if (!result.valid()) {
        const sol::error error{ result };
        spdlog::error("[{}] command '{}' failed: {}", id, name, error.what());
        services_.reply(source, id, fmt::format("'{}' failed - see the proxy log", name));
    }
    return true;
}
}
