#include "host.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <fmt/format.h>
#include <glaze/glaze.hpp>
#include <spdlog/spdlog.h>

#include "json.hpp"
#include "../core/features.hpp"
#include "../scripting/binding_module.hpp"
#include "../scripting/lua_engine.hpp"

namespace modules {
struct ModuleHost::Loaded {
    struct Command {
        std::string description;
        sol::protected_function run;
    };

    struct Timer {
        int id{ 0 };
        std::chrono::steady_clock::time_point due;
        std::chrono::milliseconds every{ 0 };   // 0: once
        sol::protected_function run;
    };

    struct Section {
        MenuSection info;
        sol::protected_function status;
    };

    Manifest manifest;
    Settings settings;
    std::filesystem::path store_file;
    Settings store;
    bool store_broken{ false };

    // Declared before everything holding Lua references: destroyed last.
    std::unique_ptr<scripting::LuaEngine> engine;
    std::map<std::string, std::vector<sol::protected_function>> handlers;
    std::map<std::string, Command> commands;
    std::vector<Timer> timers;
    int next_timer{ 1 };
    std::vector<Section> sections;
    std::map<std::string, sol::object> files;
};

namespace {
constexpr std::array EVENTS{
    std::string_view{ "right_click" },
    std::string_view{ "left_click" },
    std::string_view{ "tile_click" },
    std::string_view{ "dialog" },
    std::string_view{ "world_menu" },
    std::string_view{ "world_enter" },
    std::string_view{ "disconnect" },
    std::string_view{ "inventory" },
    std::string_view{ "object" },
    std::string_view{ "walk_end" },
};

constexpr std::array CALLS{
    std::string_view{ "OnSpawn" },
    std::string_view{ "OnRemove" },
    std::string_view{ "OnNameChanged" },
    std::string_view{ "OnDialogRequest" },
    std::string_view{ "OnSetPos" },
    std::string_view{ "OnRequestWorldSelectMenu" },
};

constexpr std::chrono::milliseconds MIN_TIMER{ 10 };

// Helpers every module gets, written in Lua: the dialog builder and the
// tags for typed numbers in function calls to the game.
constexpr std::string_view PRELUDE{ R"LUA(
local gtproxy = gtproxy
function gtproxy.u32(n) return { u32 = n } end
function gtproxy.i32(n) return { i32 = n } end
function gtproxy.f32(n) return { f32 = n } end
function gtproxy.vec2(x, y) return { x = x, y = y } end

-- The game's dialog markup (OnDialogRequest), line by line.
local Dialog = {}
Dialog.__index = Dialog
local function add(self, line) self[#self + 1] = line; return self end
function Dialog:header(title, icon)
    add(self, "set_default_color|`o\n")
    return add(self, string.format("add_label_with_icon|big|`w%s``|left|%d|\n", title, icon or 32))
end
function Dialog:section(title, icon) return add(self, string.format("add_label_with_icon|small|`w%s``|left|%d|\n", title, icon)) end
function Dialog:item(text, icon) return add(self, string.format("add_label_with_icon|small|%s|left|%d|\n", text, icon)) end
function Dialog:small(text) return add(self, string.format("add_smalltext|%s|\n", text)) end
function Dialog:text(text) return add(self, string.format("add_textbox|%s|left|\n", text)) end
function Dialog:spacer() return add(self, "add_spacer|small|\n") end
function Dialog:button(name, label) return add(self, string.format("add_button|%s|%s|noflags|0|0|\n", name, label)) end
function Dialog:checkbox(name, label, on) return add(self, string.format("add_checkbox|%s|%s|%d|\n", name, label, on and 1 or 0)) end
function Dialog:input(name, label, value, max)
    return add(self, string.format("add_text_input|%s|%s|%s|%d|\n", name, label, gtproxy.text.sanitize(tostring(value or "")), max))
end
function Dialog:finish(name, ok)
    add(self, "add_quick_exit|\n")
    add(self, string.format("end_dialog|%s|Close|%s|\n", name, ok or ""))
    return table.concat(self)
end
function gtproxy.dialog() return setmetatable({}, Dialog) end
)LUA" };

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

sol::object to_lua(sol::state_view lua, const packet::variant& value)
{
    if (const auto* text{ std::get_if<std::string>(&value) }) {
        return sol::make_object(lua, *text);
    }
    if (const auto* vec{ std::get_if<glm::vec2>(&value) }) {
        return sol::make_object(lua, lua.create_table_with("x", vec->x, "y", vec->y));
    }
    if (const auto* vec{ std::get_if<glm::vec3>(&value) }) {
        return sol::make_object(lua, lua.create_table_with("x", vec->x, "y", vec->y, "z", vec->z));
    }
    if (const auto* number{ std::get_if<float>(&value) }) {
        return sol::make_object(lua, static_cast<double>(*number));
    }
    if (const auto* number{ std::get_if<std::uint32_t>(&value) }) {
        return sol::make_object(lua, static_cast<double>(*number));
    }
    return sol::make_object(lua, static_cast<double>(std::get<std::int32_t>(value)));
}

// A Lua value as a call argument. Numbers keep the type the argument had
// ('like'), or are tagged with gtproxy.u32 / i32 / f32; plain whole numbers
// become signed, others floats.
packet::variant from_lua(const sol::object& value, const packet::variant* like, const std::size_t index)
{
    const auto number_like = [&](const double number) -> packet::variant {
        if (like) {
            if (std::holds_alternative<float>(*like)) {
                return static_cast<float>(number);
            }
            if (std::holds_alternative<std::uint32_t>(*like)) {
                return static_cast<std::uint32_t>(number);
            }
            if (std::holds_alternative<std::int32_t>(*like)) {
                return static_cast<std::int32_t>(number);
            }
        }
        if (number == std::floor(number) && number >= std::numeric_limits<std::int32_t>::min() && number <= std::numeric_limits<std::int32_t>::max()) {
            return static_cast<std::int32_t>(number);
        }
        return static_cast<float>(number);
    };

    switch (value.get_type()) {
    case sol::type::string:
        return value.as<std::string>();
    case sol::type::number:
        return number_like(value.as<double>());
    case sol::type::table: {
        const auto table{ value.as<sol::table>() };
        if (const sol::object tagged{ table["u32"] }; tagged.get_type() == sol::type::number) {
            return static_cast<std::uint32_t>(tagged.as<double>());
        }
        if (const sol::object tagged{ table["i32"] }; tagged.get_type() == sol::type::number) {
            return static_cast<std::int32_t>(tagged.as<double>());
        }
        if (const sol::object tagged{ table["f32"] }; tagged.get_type() == sol::type::number) {
            return static_cast<float>(tagged.as<double>());
        }
        const sol::object x{ table["x"] };
        const sol::object y{ table["y"] };
        const sol::object z{ table["z"] };
        if (x.get_type() == sol::type::number && y.get_type() == sol::type::number) {
            if (z.get_type() == sol::type::number) {
                return glm::vec3{ x.as<float>(), y.as<float>(), z.as<float>() };
            }
            return glm::vec2{ x.as<float>(), y.as<float>() };
        }
        break;
    }
    default:
        break;
    }
    throw std::runtime_error{ fmt::format("argument {} must be text, a number or {{ x = , y = }}", index + 1) };
}

std::vector<packet::variant> args_from_lua(const sol::table& args, const std::vector<packet::variant>* like)
{
    std::vector<packet::variant> out;
    const auto count{ args.size() };
    for (std::size_t i{ 0 }; i < count; ++i) {
        const sol::object value{ args[i + 1] };
        out.push_back(from_lua(value, like && i < like->size() ? &(*like)[i] : nullptr, i));
    }
    return out;
}

// A setting or stored value for Lua; whole numbers become Lua integers
// (350, not 350.0).
sol::object setting_object(lua_State* lua, const SettingValue& value)
{
    if (const auto* number{ std::get_if<double>(&value) };
        number && *number == std::floor(*number) && std::abs(*number) < 9.0e15) {
        return sol::make_object(lua, static_cast<lua_Integer>(*number));
    }
    return std::visit([&](const auto& v) { return sol::make_object(lua, v); }, value);
}

void save_store(ModuleHost::Loaded& module);

// The `gtproxy` table: everything a module can use (API version 2).
//
// Every function handed to Lua captures plain pointers only: sol2 frees
// captured objects with destructors (std::string ...) at the wrong place
// when the Lua state closes.
class ModuleApi final : public scripting::IBindingModule {
public:
    ModuleApi(ModuleHost::Loaded& module, Services& services, std::function<bool(std::string_view)> name_taken)
        : module_{ module }
        , services_{ services }
        , name_taken_{ std::move(name_taken) }
    { }

    [[nodiscard]] std::string_view name() const override { return "gtproxy module API"; }

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
        api["prefix"] = std::string(1, services->prefix());
        api["module"] = lua.create_table_with(
            "id", module->manifest.id,
            "version", module->manifest.version,
            "name", module->manifest.name
        );

        bind_core(lua, api, module, services, self);
        bind_world(lua, api, services);
        bind_game(lua, api, module, services);
        bind_runtime(lua, api, module);

        const Watchdog watchdog{ lua.lua_state() };
        lua.safe_script(PRELUDE, "@gtproxy prelude");
    }

    std::function<bool(std::string_view)> name_taken_;

private:
    // API 1: events, commands, settings, reachability, map window, log.
    static void bind_core(sol::state& lua, sol::table& api, ModuleHost::Loaded* module, Services* services, ModuleApi* self)
    {
        sol::table log{ lua.create_table() };
        log.set_function("info", [module](const sol::object& text) { spdlog::info("[{}] {}", module->manifest.id, as_text(text)); });
        log.set_function("warn", [module](const sol::object& text) { spdlog::warn("[{}] {}", module->manifest.id, as_text(text)); });
        log.set_function("error", [module](const sol::object& text) { spdlog::error("[{}] {}", module->manifest.id, as_text(text)); });
        api["log"] = log;

        api.set_function("on", [module](const std::string& event, const sol::object& handler) {
            if (!known_event(event)) {
                throw std::runtime_error{ fmt::format("gtproxy.on: unknown event '{}' (see docs/modules.md)", event) };
            }
            if (handler.get_type() != sol::type::function) {
                throw std::runtime_error{ "gtproxy.on: the handler must be a function" };
            }
            module->handlers[event].push_back(handler.as<sol::protected_function>());
        });

        api.set_function("hook_call", [module](const std::string& name, const sol::object& handler) {
            if (!hookable_call(name)) {
                throw std::runtime_error{ fmt::format("gtproxy.hook_call: '{}' cannot be hooked (see docs/modules.md)", name) };
            }
            if (handler.get_type() != sol::type::function) {
                throw std::runtime_error{ "gtproxy.hook_call: the handler must be a function" };
            }
            module->handlers["call:" + name].push_back(handler.as<sol::protected_function>());
        });

        api.set_function("command", [module, self](const std::string& name, const std::string& description, const sol::object& run) {
            if (!valid_command_name(name)) {
                throw std::runtime_error{ fmt::format("gtproxy.command: '{}' is not a valid command name (a-z, 0-9, '_')", name) };
            }
            if (run.get_type() != sol::type::function) {
                throw std::runtime_error{ "gtproxy.command: the command must be a function" };
            }
            if (module->commands.contains(name) || self->name_taken_(name)) {
                spdlog::error("[{}] gtproxy.command: '{}' is already a command of the proxy, a script or another module; not added", module->manifest.id, name);
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
            return setting_object(state, it->second);
        });

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
            card.toast = options.get_or("toast", false);
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

        sol::table text{ lua.create_table() };
        text.set_function("strip_colors", [](const std::string& value) { return core::features::strip_colors(value); });
        text.set_function("sanitize", [](const std::string& value) { return core::features::sanitize(value); });
        api["text"] = text;
    }

    // World, players, inventory and items.
    static void bind_world(sol::state& lua, sol::table& api, Services* services)
    {
        sol::table world{ lua.create_table() };
        world.set_function("info", [services](sol::this_state state) {
            sol::state_view view_lua{ state };
            const auto view{ services->world() };
            sol::table info{ view_lua.create_table() };
            info["connected"] = view.connected;
            info["in_world"] = view.in_world;
            info["name"] = view.name;
            info["previous"] = view.previous;
            sol::table recent{ view_lua.create_table() };
            for (std::size_t i{ 0 }; i < view.recent.size(); ++i) {
                recent[i + 1] = view.recent[i];
            }
            info["recent"] = recent;
            info["width"] = view.width;
            info["height"] = view.height;
            info["net_id"] = view.net_id;
            info["real_name"] = view.real_name;
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

        world.set_function("players", [services](sol::this_state state) {
            sol::state_view view_lua{ state };
            sol::table list{ view_lua.create_table() };
            std::size_t i{ 0 };
            for (const auto& player : services->players()) {
                sol::table entry{ view_lua.create_table() };
                entry["net_id"] = player.net_id;
                entry["user_id"] = player.user_id;
                entry["name"] = player.name;
                entry["country"] = player.country;
                entry["invisible"] = player.invisible;
                entry["is_local"] = player.local;
                entry["x"] = player.position.x;
                entry["y"] = player.position.y;
                entry["tile_x"] = player.position.tile_x;
                entry["tile_y"] = player.position.tile_y;
                list[++i] = entry;
            }
            return list;
        });

        world.set_function("objects", [services](sol::this_state state) {
            sol::state_view view_lua{ state };
            sol::table list{ view_lua.create_table() };
            std::size_t i{ 0 };
            for (const auto& object : services->objects()) {
                list[++i] = view_lua.create_table_with(
                    "object_id", object.object_id,
                    "item", object.item_id,
                    "amount", object.amount,
                    "x", object.x,
                    "y", object.y
                );
            }
            return list;
        });

        world.set_function("tile", [services](sol::this_state state, const double x, const double y) -> sol::object {
            const auto tile{ services->tile({ whole_number(x, "gtproxy.world.tile: x"), whole_number(y, "gtproxy.world.tile: y") }) };
            if (!tile) {
                return sol::make_object(state, sol::lua_nil);
            }
            sol::state_view view_lua{ state };
            return sol::make_object(state, view_lua.create_table_with(
                "fg", tile->foreground,
                "bg", tile->background,
                "flags", tile->flags,
                "cell", tile->cell,
                "text", tile->text
            ));
        });
        api["world"] = world;

        sol::table inventory{ lua.create_table() };
        inventory.set_function("info", [services](sol::this_state state) {
            sol::state_view view_lua{ state };
            const auto view{ services->inventory() };
            sol::table items{ view_lua.create_table() };
            std::size_t i{ 0 };
            for (const auto& item : view.items) {
                items[++i] = view_lua.create_table_with("id", item.id, "count", item.count, "flags", item.flags);
            }
            return view_lua.create_table_with("known", view.known, "slots", view.slots, "items", items);
        });
        inventory.set_function("count", [services](const double id) {
            const auto view{ services->inventory() };
            const auto wanted{ whole_number(id, "gtproxy.inventory.count: id") };
            for (const auto& item : view.items) {
                if (item.id == wanted) {
                    return static_cast<int>(item.count);
                }
            }
            return 0;
        });
        api["inventory"] = inventory;

        const auto item_table = [](sol::state_view view_lua, const ItemView& item) {
            return view_lua.create_table_with(
                "id", item.id,
                "name", item.name,
                "type", item.type,
                "rarity", item.rarity,
                "max_amount", item.max_amount,
                "untradeable", item.untradeable
            );
        };
        sol::table items{ lua.create_table() };
        items.set_function("loaded", [services] { return services->items_loaded(); });
        items.set_function("get", [services, item_table](sol::this_state state, const double id) -> sol::object {
            const auto item{ services->item(static_cast<std::uint32_t>(whole_number(id, "gtproxy.items.get: id"))) };
            if (!item) {
                return sol::make_object(state, sol::lua_nil);
            }
            return sol::make_object(state, item_table(state, *item));
        });
        items.set_function("find", [services, item_table](sol::this_state state, const std::string& query, const sol::object& limit) {
            sol::state_view view_lua{ state };
            const std::size_t max{ limit.get_type() == sol::type::number ? static_cast<std::size_t>(std::max(1.0, limit.as<double>())) : 12 };
            sol::table list{ view_lua.create_table() };
            std::size_t i{ 0 };
            for (const auto& item : services->find_items(query, max)) {
                list[++i] = item_table(state, item);
            }
            return list;
        });
        api["items"] = items;
    }

    // Talking to the game and the server, walking.
    static void bind_game(sol::state& lua, sol::table& api, ModuleHost::Loaded* module, Services* services)
    {
        sol::table game{ lua.create_table() };
        game.set_function("message", [services](const sol::object& text) { services->game_message(as_text(text)); });
        game.set_function("dialog", [services](const std::string& markup) { services->game_dialog(markup); });
        game.set_function("call", [services](const std::string& name, const double net_id, const sol::variadic_args& args) {
            CallOut call{};
            call.name = name;
            call.net_id = whole_number(net_id, "gtproxy.game.call: net_id");
            std::size_t index{ 0 };
            for (const auto& arg : args) {
                call.args.push_back(from_lua(arg, nullptr, index++));
            }
            return services->game_call(call);
        });
        api["game"] = game;

        sol::table server{ lua.create_table() };
        server.set_function("text", [services](const std::string& text) { return services->server_text(text, false); });
        server.set_function("game_text", [services](const std::string& text) { return services->server_text(text, true); });
        api["server"] = server;

        const auto walk_table = [](sol::state_view view_lua, const WalkView& walk) {
            sol::table table{ view_lua.create_table_with("ok", walk.ok, "message", walk.message) };
            if (walk.tile) {
                table["x"] = walk.tile->x;
                table["y"] = walk.tile->y;
            }
            return table;
        };
        sol::table walk{ lua.create_table() };
        walk.set_function("to", [services, walk_table](sol::this_state state, const double x, const double y) {
            return walk_table(state, services->walk_to({ whole_number(x, "gtproxy.walk.to: x"), whole_number(y, "gtproxy.walk.to: y") }));
        });
        walk.set_function("stop", [services, walk_table](sol::this_state state) { return walk_table(state, services->walk_stop()); });
        walk.set_function("active", [services] { return services->walking(); });
        api["walk"] = walk;

        sol::table menu{ lua.create_table() };
        menu.set_function("add", [module](const sol::object& value) {
            if (value.get_type() != sol::type::table) {
                throw std::runtime_error{ "gtproxy.menu.add: expects a table { title = , icon = , button = , label = , order = , status = }" };
            }
            const auto options{ value.as<sol::table>() };
            ModuleHost::Loaded::Section section{};
            section.info.module = module->manifest.id;
            section.info.title = options.get_or<std::string>("title", module->manifest.id);
            section.info.icon = options.get_or("icon", 32);
            section.info.button = options.get_or<std::string>("button", "");
            section.info.label = options.get_or<std::string>("label", "");
            section.info.order = options.get_or("order", 100);
            if (section.info.button.empty()) {
                throw std::runtime_error{ "gtproxy.menu.add: 'button' is missing (the button that opens the page)" };
            }
            if (const sol::object status{ options["status"] }; status.get_type() == sol::type::function) {
                section.status = status.as<sol::protected_function>();
            }
            module->sections.push_back(std::move(section));
        });
        api["menu"] = menu;
    }

    // Timers, other files of the module, stored data.
    static void bind_runtime(sol::state& lua, sol::table& api, ModuleHost::Loaded* module)
    {
        const auto add_timer = [module](const double ms, const sol::object& run, const bool repeat) {
            if (run.get_type() != sol::type::function) {
                throw std::runtime_error{ "gtproxy.after / every: the second argument must be a function" };
            }
            const std::chrono::milliseconds delay{ std::max<long long>(static_cast<long long>(ms), MIN_TIMER.count()) };
            const int id{ module->next_timer++ };
            module->timers.push_back({ id, std::chrono::steady_clock::now() + delay, repeat ? delay : std::chrono::milliseconds{ 0 }, run.as<sol::protected_function>() });
            return id;
        };
        api.set_function("after", [add_timer](const double ms, const sol::object& run) { return add_timer(ms, run, false); });
        api.set_function("every", [add_timer](const double ms, const sol::object& run) { return add_timer(ms, run, true); });
        // Milliseconds on a steady clock (only differences mean anything).
        api.set_function("now", [] {
            return static_cast<lua_Integer>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        });
        api.set_function("cancel", [module](const sol::object& id) {
            if (id.get_type() != sol::type::number) {
                return false;   // gtproxy.cancel(nil) is fine
            }
            const auto wanted{ static_cast<int>(id.as<double>()) };
            return std::erase_if(module->timers, [&](const ModuleHost::Loaded::Timer& timer) { return timer.id == wanted; }) > 0;
        });

        // Another Lua file of the module's folder, run once (like require).
        api.set_function("load", [module](sol::this_state state, const std::string& file) {
            if (const auto it{ module->files.find(file) }; it != module->files.end()) {
                return it->second;
            }
            const std::filesystem::path relative{ file };
            const bool inside{
                relative.is_relative() && !relative.has_root_name() && relative.extension() == ".lua"
                && std::ranges::none_of(relative, [](const std::filesystem::path& part) { return part == ".."; })
            };
            if (!inside) {
                throw std::runtime_error{ fmt::format("gtproxy.load: '{}' must be a .lua file inside the module's folder", file) };
            }
            const auto code{ json::read_text(module->manifest.dir / relative) };
            if (!code) {
                throw std::runtime_error{ fmt::format("gtproxy.load: cannot read {}", file) };
            }
            sol::state_view view_lua{ state };
            const auto chunk_name{ fmt::format("@modules/{}/{}", module->manifest.dir.filename().generic_string(), relative.generic_string()) };
            sol::protected_function_result result{ view_lua.safe_script(*code, sol::script_pass_on_error, chunk_name) };
            if (!result.valid()) {
                const sol::error error{ result };
                throw std::runtime_error{ error.what() };
            }
            sol::object value{ result.get_type() == sol::type::none ? sol::make_object(state, true) : result.get<sol::object>() };
            module->files[file] = value;
            return value;
        });

        sol::table store{ lua.create_table() };
        store.set_function("get", [module](sol::this_state state, const std::string& key, const sol::object& fallback) -> sol::object {
            const auto it{ module->store.find(key) };
            if (it == module->store.end()) {
                return fallback;
            }
            return setting_object(state, it->second);
        });
        store.set_function("set", [module](const std::string& key, const sol::object& value) {
            switch (value.get_type()) {
            case sol::type::lua_nil:
            case sol::type::none:
                module->store.erase(key);
                break;
            case sol::type::boolean:
                module->store[key] = value.as<bool>();
                break;
            case sol::type::number:
                module->store[key] = value.as<double>();
                break;
            case sol::type::string:
                module->store[key] = value.as<std::string>();
                break;
            default:
                throw std::runtime_error{ "gtproxy.store.set: a value is true/false, a number, text or nil" };
            }
            save_store(*module);
        });
        api["store"] = store;
    }

    ModuleHost::Loaded& module_;
    Services& services_;
};

void load_store(ModuleHost::Loaded& module)
{
    std::error_code ec;
    if (!std::filesystem::exists(module.store_file, ec)) {
        return;
    }
    const auto text{ json::read_text(module.store_file) };
    auto parsed{ text ? json::parse<Settings>(*text) : std::unexpected{ std::string{ "cannot be read" } } };
    if (!parsed) {
        spdlog::error("Module '{}': its data file {} is broken ({}); starting without it", module.manifest.id, module.store_file.string(), parsed.error());
        module.store_broken = true;
        return;
    }
    module.store = std::move(*parsed);
}

void save_store(ModuleHost::Loaded& module)
{
    std::error_code ec;
    std::filesystem::create_directories(module.store_file.parent_path(), ec);
    if (module.store_broken) {
        // Keep the broken file for the user instead of overwriting it.
        auto backup{ module.store_file };
        backup += ".broken";
        std::filesystem::rename(module.store_file, backup, ec);
        module.store_broken = false;
    }

    std::string json;
    if (glz::write<glz::opts{ .prettify = true }>(module.store, json)) {
        return;
    }
    std::ofstream out{ module.store_file, std::ios::binary | std::ios::trunc };
    out << json;
    if (!out) {
        spdlog::warn("Module '{}': could not save {}", module.manifest.id, module.store_file.string());
    }
}

// Calls one Lua handler under the watchdog; logs a failure.
std::optional<sol::protected_function_result> call_guarded(
    const ModuleHost::Loaded& module,
    const sol::protected_function& handler,
    const std::string_view what,
    const auto&... args
)
{
    const Watchdog watchdog{ module.engine->state().lua_state() };
    sol::protected_function_result result{ handler(args...) };
    if (!result.valid()) {
        const sol::error error{ result };
        spdlog::error("[{}] {} failed: {}", module.manifest.id, what, error.what());
        return std::nullopt;
    }
    return result;
}

std::string returned_word(const sol::protected_function_result& result)
{
    if (result.return_count() == 0) {
        return {};
    }
    const sol::object value{ result.get<sol::object>() };
    return value.get_type() == sol::type::string ? value.as<std::string>() : std::string{};
}
}

bool known_event(const std::string_view event)
{
    return std::ranges::find(EVENTS, event) != EVENTS.end();
}

bool hookable_call(const std::string_view name)
{
    return std::ranges::find(CALLS, name) != CALLS.end();
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
    module->store_file = (data_dir_.empty() ? manifest.dir : data_dir_) / fmt::format("{}.json", manifest.id);
    load_store(*module);

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
    std::string events;
    for (const auto& [event, handlers] : module->handlers) {
        events += (events.empty() ? "" : ", ") + event;
    }
    spdlog::info(
        "Module '{}' {} loaded from {}: events: {} | commands: {}",
        manifest.id,
        manifest.version,
        "modules/" + manifest.dir.filename().generic_string(),
        events.empty() ? std::string{ "none" } : events,
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

ModuleHost::Outcome ModuleHost::emit(const std::string_view event, const Fields& fields)
{
    Outcome outcome{};
    const std::string key{ event };
    for (const auto& module : loaded_) {
        const auto it{ module->handlers.find(key) };
        if (it == module->handlers.end()) {
            continue;
        }
        for (const auto& handler : it->second) {
            sol::table table{ module->engine->state().create_table() };
            if (fields) {
                fields(table);
            }
            ++outcome.ran;
            const auto result{ call_guarded(*module, handler, fmt::format("{} handler", event), table) };
            if (result && returned_word(*result) == "cancel") {
                outcome.cancelled = true;
            }
        }
    }
    return outcome;
}

std::size_t ModuleHost::right_click(const map::TilePos tile, const bool in_world)
{
    return emit("right_click", [&](sol::table& event) {
        event["x"] = tile.x;
        event["y"] = tile.y;
        event["in_world"] = in_world;
    }).ran;
}

ModuleHost::Outcome ModuleHost::hook_call(Call& call)
{
    Outcome outcome{};
    const std::string key{ "call:" + call.name };
    for (const auto& module : loaded_) {
        const auto it{ module->handlers.find(key) };
        if (it == module->handlers.end()) {
            continue;
        }
        for (const auto& handler : it->second) {
            sol::state& lua{ module->engine->state() };
            sol::table args{ lua.create_table() };
            for (std::size_t i{ 0 }; i < call.args.size(); ++i) {
                args[i + 1] = to_lua(lua, call.args[i]);
            }
            sol::table table{ lua.create_table_with("name", call.name, "net_id", call.net_id, "args", args) };

            ++outcome.ran;
            const auto result{ call_guarded(*module, handler, fmt::format("{} hook", call.name), table) };
            if (!result) {
                continue;
            }

            const auto word{ returned_word(*result) };
            if (word == "cancel") {
                outcome.cancelled = true;
                return outcome;
            }
            if (word == "replace") {
                try {
                    call.args = args_from_lua(table["args"], &call.args);
                    call.net_id = static_cast<std::int32_t>(table.get_or("net_id", static_cast<double>(call.net_id)));
                    outcome.replaced = true;
                    return outcome;
                }
                catch (const std::exception& e) {
                    spdlog::error("[{}] {} hook: {}; the call goes on unchanged", module->manifest.id, call.name, e.what());
                }
            }
        }
    }
    return outcome;
}

void ModuleHost::update()
{
    const auto now{ std::chrono::steady_clock::now() };
    for (const auto& module : loaded_) {
        std::vector<int> due;
        for (const auto& timer : module->timers) {
            if (timer.due <= now) {
                due.push_back(timer.id);
            }
        }

        for (const int id : due) {
            const auto it{ std::ranges::find_if(module->timers, [&](const Loaded::Timer& timer) { return timer.id == id; }) };
            if (it == module->timers.end()) {
                continue; // cancelled by an earlier timer
            }

            // A copy: the callback may add or cancel timers.
            const auto run{ it->run };
            const bool repeat{ it->every.count() > 0 };
            if (repeat) {
                it->due = now + it->every;
            }
            else {
                module->timers.erase(it);
            }

            if (!call_guarded(*module, run, "timer") && repeat) {
                std::erase_if(module->timers, [&](const Loaded::Timer& timer) { return timer.id == id; });
                spdlog::warn("[{}] a repeating timer failed and was stopped", module->manifest.id);
            }
        }
    }
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

    sol::table context{ lua.create_table() };
    context["source"] = source == Source::Game ? "game" : "console";
    context["name"] = std::string{ name };
    // ctx.reply(text) and ctx:reply(text) both work. Plain pointers only
    // in the capture (see ModuleApi).
    auto* const services{ &services_ };
    context.set_function("reply", [services, source, module](const sol::variadic_args& args) {
        for (const auto& arg : args) {
            if (arg.get_type() != sol::type::table) {
                services->reply(source, module->manifest.id, as_text(arg));
                return;
            }
        }
    });

    if (!call_guarded(*module, module->commands.at(std::string{ name }).run, fmt::format("command '{}'", name), arguments, context)) {
        services_.reply(source, module->manifest.id, fmt::format("'{}' failed - see the proxy log", name));
    }
    return true;
}

std::vector<ModuleHost::MenuSection> ModuleHost::menu_sections() const
{
    std::vector<MenuSection> sections;
    for (const auto& module : loaded_) {
        for (const auto& section : module->sections) {
            sections.push_back(section.info);
        }
    }
    return sections;
}

std::vector<std::string> ModuleHost::menu_status()
{
    std::vector<std::string> lines;
    for (const auto& module : loaded_) {
        for (const auto& section : module->sections) {
            if (!section.status.valid()) {
                continue;
            }
            const auto result{ call_guarded(*module, section.status, "menu status") };
            if (!result || result->return_count() == 0) {
                continue;
            }
            const sol::object value{ result->get<sol::object>() };
            if (value.get_type() == sol::type::string && !value.as<std::string>().empty()) {
                lines.push_back(value.as<std::string>());
            }
            else if (value.get_type() == sol::type::table) {
                for (const auto& [index, line] : value.as<sol::table>()) {
                    if (line.get_type() == sol::type::string) {
                        lines.push_back(line.as<std::string>());
                    }
                }
            }
        }
    }
    return lines;
}
}
