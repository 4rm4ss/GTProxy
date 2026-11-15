#include "host.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
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
    // gtproxy.export: what other modules may call (gtproxy.call).
    std::map<std::string, sol::protected_function> exports;
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
    std::string_view{ "settings" },
    std::string_view{ "wheel" },
    std::string_view{ "tile" },
    std::string_view{ "player_join" },
    std::string_view{ "player_leave" },
    std::string_view{ "console" },
    std::string_view{ "pick" },
    std::string_view{ "panel" },
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

-- A dialog the server sent (OnDialogRequest markup), read like the game.
gtproxy.ui = {}
local function split(text, sep)
    local parts, start = {}, 1
    while true do
        local at = string.find(text, sep, start, true)
        if not at then parts[#parts + 1] = string.sub(text, start); return parts end
        parts[#parts + 1] = string.sub(text, start, at - 1)
        start = at + 1
    end
end
gtproxy.ui.split = split
function gtproxy.ui.parse_dialog(markup)
    local d = { name = "", embed = {}, embed_list = {}, inputs = {}, input_list = {}, checkboxes = {},
        checkbox_list = {}, buttons = {}, button_list = {}, texts = {}, icons = {}, pickers = {} }
    for line in (markup .. "\n"):gmatch("(.-)\n") do
        local p = split(line, "|")
        local key = p[1]
        if key == "end_dialog" then d.name = p[2] or ""
        elseif key == "embed_data" and p[2] then
            d.embed[p[2]] = p[3] or ""
            d.embed_list[#d.embed_list + 1] = { p[2], p[3] or "" }
        elseif key == "add_text_input" and p[2] then
            d.inputs[p[2]] = p[4] or ""
            d.input_list[#d.input_list + 1] = p[2]
            d.texts[#d.texts + 1] = p[3] or ""
        elseif key == "add_checkbox" and p[2] then
            d.checkboxes[p[2]] = p[4] == "1"
            d.checkbox_list[#d.checkbox_list + 1] = p[2]
            d.texts[#d.texts + 1] = p[3] or ""
        elseif key == "add_button" and p[2] then
            d.buttons[p[2]] = p[3] or ""
            d.button_list[#d.button_list + 1] = p[2]
        elseif key == "add_player_picker" and p[2] then
            d.pickers[#d.pickers + 1] = p[2]
        elseif key == "add_label_with_icon" or key == "add_label" or key == "add_textbox" or key == "add_smalltext" then
            local text = key == "add_label_with_icon" and p[3] or p[2]
            if key == "add_label" then text = p[3] or p[2] end
            d.texts[#d.texts + 1] = text or ""
            if key == "add_label_with_icon" then d.icons[#d.icons + 1] = tonumber(p[5] or "") end
        end
    end
    d.text = gtproxy.text.strip_colors(table.concat(d.texts, "\n"))
    return d
end
-- The game's answer to such a dialog: its embedded data, its inputs (values
-- given here, else as the dialog had them) and the button pressed (nil: OK).
function gtproxy.ui.answer(d, values, button)
    values = values or {}
    local lines = { "action|dialog_return", "dialog_name|" .. d.name }
    for _, e in ipairs(d.embed_list) do lines[#lines + 1] = e[1] .. "|" .. e[2] .. "|" end
    for _, name in ipairs(d.input_list) do
        local v = values[name]
        if v == nil then v = d.inputs[name] end
        lines[#lines + 1] = name .. "|" .. gtproxy.text.sanitize(tostring(v))
    end
    for _, name in ipairs(d.checkbox_list) do
        local v = values[name]
        if v == nil then v = d.checkboxes[name] end
        lines[#lines + 1] = name .. "|" .. ((v == true or v == "1" or v == 1) and "1" or "0")
    end
    for name, v in pairs(values) do
        if d.inputs[name] == nil and d.checkboxes[name] == nil and d.embed[name] == nil then
            lines[#lines + 1] = name .. "|" .. gtproxy.text.sanitize(tostring(v))
        end
    end
    if button then lines[#lines + 1] = "buttonClicked|" .. button end
    return table.concat(lines, "\n") .. "\n"
end
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

void json_text(std::string& out, const std::string_view text)
{
    out += '"';
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out += fmt::format("\\u{:04x}", static_cast<unsigned>(c));
            }
            else {
                out += c;
            }
        }
    }
    out += '"';
}

// A Lua value as JSON: tables with keys 1..n are arrays, other tables
// objects; functions and the like become null.
void to_json(std::string& out, const sol::object& value, const int depth)
{
    if (depth > 16) {
        out += "null";
        return;
    }
    switch (value.get_type()) {
    case sol::type::boolean:
        out += value.as<bool>() ? "true" : "false";
        return;
    case sol::type::number: {
        const double number{ value.as<double>() };
        if (!std::isfinite(number)) {
            out += "null";
        }
        else if (number == std::floor(number) && std::abs(number) < 9.0e15) {
            out += fmt::format("{}", static_cast<long long>(number));
        }
        else {
            out += fmt::format("{}", number);
        }
        return;
    }
    case sol::type::string:
        json_text(out, value.as<std::string>());
        return;
    case sol::type::table: {
        const auto table{ value.as<sol::table>() };
        const std::size_t length{ table.size() };
        std::size_t entries{ 0 };
        for (const auto& entry : table) {
            std::ignore = entry;
            ++entries;
        }
        if (length > 0 && entries == length) {
            out += '[';
            for (std::size_t i{ 1 }; i <= length; ++i) {
                if (i > 1) {
                    out += ',';
                }
                to_json(out, table[i], depth + 1);
            }
            out += ']';
            return;
        }
        out += '{';
        bool first{ true };
        for (const auto& [key, item] : table) {
            if (key.get_type() != sol::type::string && key.get_type() != sol::type::number) {
                continue;
            }
            if (!first) {
                out += ',';
            }
            first = false;
            json_text(out, key.get_type() == sol::type::string ? key.as<std::string>() : as_text(key));
            out += ':';
            to_json(out, item, depth + 1);
        }
        out += '}';
        return;
    }
    default:
        out += "null";
        return;
    }
}

// A value of one module's Lua state rebuilt in another's (true/false,
// numbers, text and tables of them; anything else becomes nil).
sol::object copy_value(const sol::object& value, sol::state_view to, const int depth)
{
    switch (value.get_type()) {
    case sol::type::boolean:
        return sol::make_object(to, value.as<bool>());
    case sol::type::number:
        if (value.is<lua_Integer>()) {
            return sol::make_object(to, value.as<lua_Integer>());
        }
        return sol::make_object(to, value.as<double>());
    case sol::type::string:
        return sol::make_object(to, value.as<std::string>());
    case sol::type::table: {
        if (depth > 16) {
            return sol::make_object(to, sol::lua_nil);
        }
        sol::table copy{ to.create_table() };
        for (const auto& [key, item] : value.as<sol::table>()) {
            const auto copied_key{ copy_value(key, to, depth + 1) };
            if (copied_key.get_type() != sol::type::lua_nil) {
                copy[copied_key] = copy_value(item, to, depth + 1);
            }
        }
        return sol::make_object(to, copy);
    }
    default:
        return sol::make_object(to, sol::lua_nil);
    }
}

std::optional<Overlay> overlay_from(const sol::object& value)
{
    if (value.get_type() != sol::type::table) {
        return std::nullopt;
    }
    const auto spec{ value.as<sol::table>() };
    Overlay overlay{};
    const auto tile_of = [](const sol::table& t) { return map::TilePos{ static_cast<int>(t.get_or("x", 0.0)), static_cast<int>(t.get_or("y", 0.0)) }; };
    if (const sol::object markers{ spec["markers"] }; markers.get_type() == sol::type::table) {
        for (const auto& [index, item] : markers.as<sol::table>()) {
            if (item.get_type() == sol::type::table) {
                const auto t{ item.as<sol::table>() };
                const auto at{ tile_of(t) };
                overlay.markers.push_back({ at.x, at.y, t.get_or<std::string>("label", ""), t.get_or<std::string>("color", "#ffcf4a") });
            }
        }
    }
    if (const sol::object route{ spec["route"] }; route.get_type() == sol::type::table) {
        const auto t{ route.as<sol::table>() };
        for (std::size_t i{ 1 }; i <= t.size(); ++i) {
            const sol::object point{ t[i] };
            if (point.get_type() == sol::type::table) {
                overlay.route.push_back(tile_of(point.as<sol::table>()));
            }
        }
    }
    overlay.route_color = spec.get_or<std::string>("route_color", "#c77dff");
    if (const sol::object areas{ spec["areas"] }; areas.get_type() == sol::type::table) {
        for (const auto& [index, item] : areas.as<sol::table>()) {
            if (item.get_type() == sol::type::table) {
                const auto t{ item.as<sol::table>() };
                overlay.areas.push_back({
                    static_cast<int>(t.get_or("x1", 0.0)), static_cast<int>(t.get_or("y1", 0.0)),
                    static_cast<int>(t.get_or("x2", 0.0)), static_cast<int>(t.get_or("y2", 0.0)),
                    t.get_or<std::string>("label", ""), t.get_or<std::string>("color", "#4fc3ff"),
                });
            }
        }
    }
    return overlay;
}

// The `gtproxy` table: everything a module can use (API version 3).
//
// Every function handed to Lua captures plain pointers only: sol2 frees
// captured objects with destructors (std::string ...) at the wrong place
// when the Lua state closes.
class ModuleApi final : public scripting::IBindingModule {
public:
    using Caller = std::function<sol::object(sol::this_state state, const std::string& target, const std::string& name, const sol::variadic_args& args)>;

    ModuleApi(ModuleHost::Loaded& module, Services& services, std::function<bool(std::string_view)> name_taken, Caller caller)
        : name_taken_{ std::move(name_taken) }
        , caller_{ std::move(caller) }
        , module_{ module }
        , services_{ services }
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
        bind_map(lua, api, module, services, self);

        const Watchdog watchdog{ lua.lua_state() };
        lua.safe_script(PRELUDE, "@gtproxy prelude");
    }

    std::function<bool(std::string_view)> name_taken_;
    Caller caller_;

private:
    // API 1: events, commands, settings, reachability, map window, log.
    static void bind_core(sol::state& lua, sol::table& api, ModuleHost::Loaded* module, Services* services, ModuleApi* self)
    {
        sol::table log{ lua.create_table() };
        log.set_function("debug", [module](const sol::object& text) { spdlog::debug("[{}] {}", module->manifest.id, as_text(text)); });
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

        // Changes a setting from the module's page: written to
        // config/modules/<id>.json. Returns true, or false and why not.
        api.set_function("save_setting", [module, services](const std::string& key, const sol::object& value) {
            const auto it{ module->manifest.settings.find(key) };
            if (it == module->manifest.settings.end()) {
                throw std::runtime_error{ fmt::format("gtproxy.save_setting: '{}' is not a setting of this module (module.json \"settings\")", key) };
            }
            SettingValue setting{};
            if (std::holds_alternative<bool>(it->second) && value.get_type() == sol::type::boolean) {
                setting = value.as<bool>();
            }
            else if (std::holds_alternative<double>(it->second) && value.get_type() == sol::type::number) {
                setting = value.as<double>();
            }
            else if (std::holds_alternative<std::string>(it->second) && value.get_type() == sol::type::string) {
                setting = value.as<std::string>();
            }
            else {
                throw std::runtime_error{ fmt::format("gtproxy.save_setting: '{}' must be {}", key, type_name(it->second)) };
            }
            const auto problem{ services->save_setting(module->manifest.id, key, setting) };
            return std::make_tuple(problem.empty(), problem);
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
            info["server"] = view.server;
            info["user_id"] = view.user_id;
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
            sol::table info{ view_lua.create_table_with(
                "fg", tile->foreground,
                "bg", tile->background,
                "flags", tile->flags,
                "cell", tile->cell,
                "text", tile->text,
                "extra", tile->extra,
                "label", tile->label
            ) };
            if (tile->extra == "vending") {
                info["vend_item"] = tile->vend_item;
                info["vend_price"] = tile->vend_price;
            }
            if (tile->extra == "lock") {
                info["lock_owner"] = tile->lock_owner;
                info["lock_access"] = sol::as_table(tile->lock_access);
            }
            return sol::make_object(state, info);
        });

        world.set_function("vendings", [services](sol::this_state state) {
            sol::state_view view_lua{ state };
            sol::table list{ view_lua.create_table() };
            std::size_t i{ 0 };
            for (const auto& vending : services->vendings()) {
                list[++i] = view_lua.create_table_with(
                    "x", vending.x, "y", vending.y, "machine", vending.machine, "item", vending.item, "price", vending.price
                );
            }
            return list;
        });

        world.set_function("locks", [services](sol::this_state state) {
            sol::state_view view_lua{ state };
            sol::table list{ view_lua.create_table() };
            std::size_t i{ 0 };
            for (const auto& lock : services->locks()) {
                sol::table access{ view_lua.create_table() };
                for (std::size_t k{ 0 }; k < lock.access.size(); ++k) {
                    access[k + 1] = lock.access[k];
                }
                list[++i] = view_lua.create_table_with(
                    "x", lock.x, "y", lock.y, "item", lock.item, "owner", lock.owner, "access", access, "world", lock.world
                );
            }
            return list;
        });

        world.set_function("summary", [services](sol::this_state state) {
            sol::state_view view_lua{ state };
            const auto summary{ services->world_summary() };
            sol::table foreground{ view_lua.create_table() };
            sol::table background{ view_lua.create_table() };
            for (const auto& [id, count] : summary.foreground) {
                foreground[id] = count;
            }
            for (const auto& [id, count] : summary.background) {
                background[id] = count;
            }
            return view_lua.create_table_with("foreground", foreground, "background", background);
        });

        world.set_function("find_tiles", [services](sol::this_state state, const double id) {
            sol::state_view view_lua{ state };
            sol::table list{ view_lua.create_table() };
            std::size_t i{ 0 };
            for (const auto& hit : services->find_tiles(static_cast<std::uint16_t>(whole_number(id, "gtproxy.world.find_tiles: id")))) {
                list[++i] = view_lua.create_table_with("x", hit.x, "y", hit.y, "background", hit.background);
            }
            return list;
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
        server.set_function("tile_action", [services](const double x, const double y, const double item) {
            return services->tile_action({ whole_number(x, "gtproxy.server.tile_action: x"), whole_number(y, "gtproxy.server.tile_action: y") },
                static_cast<std::uint32_t>(whole_number(item, "gtproxy.server.tile_action: item")));
        });
        // The wrench on a tile: what the game sends to open a machine's or a lock's dialog.
        server.set_function("wrench", [services](const double x, const double y) {
            return services->tile_action({ whole_number(x, "gtproxy.server.wrench: x"), whole_number(y, "gtproxy.server.wrench: y") }, 32);
        });
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
        walk.set_function("to", [services, walk_table](sol::this_state state, const double x, const double y, const sol::object& options) {
            std::optional<bool> gravity;
            if (options.get_type() == sol::type::table) {
                const sol::object value{ options.as<sol::table>()["gravity"] };
                if (value.get_type() == sol::type::boolean) {
                    gravity = value.as<bool>();
                }
            }
            return walk_table(state, services->walk_to({ whole_number(x, "gtproxy.walk.to: x"), whole_number(y, "gtproxy.walk.to: y") }, gravity));
        });
        walk.set_function("stop", [services, walk_table](sol::this_state state) { return walk_table(state, services->walk_stop()); });
        walk.set_function("active", [services] { return services->walking(); });
        // gtproxy.walk.options() -> the settings in force;
        // gtproxy.walk.options({ speed = 500, ... }) changes those given.
        walk.set_function("options", [services](sol::this_state state, const sol::object& changes) {
            auto options{ services->walk_options() };
            if (changes.get_type() == sol::type::table) {
                const auto table{ changes.as<sol::table>() };
                options.speed = static_cast<float>(table.get_or("speed", static_cast<double>(options.speed)));
                options.step_ms = whole_number(table.get_or("step_ms", static_cast<double>(options.step_ms)), "gtproxy.walk.options: step_ms");
                options.gravity = table.get_or("gravity", options.gravity);
                options.jump_height = whole_number(table.get_or("jump_height", static_cast<double>(options.jump_height)), "gtproxy.walk.options: jump_height");
                options.air_jumps = whole_number(table.get_or("air_jumps", static_cast<double>(options.air_jumps)), "gtproxy.walk.options: air_jumps");
                options.doors = table.get_or<std::string>("doors", options.doors);
                if (options.doors != "access" && options.doors != "all" && options.doors != "none") {
                    throw std::runtime_error{ fmt::format("gtproxy.walk.options: doors '{}' must be \"access\", \"all\" or \"none\"", options.doors) };
                }
                options = services->set_walk_options(options);
            }
            sol::state_view view_lua{ state };
            return view_lua.create_table_with(
                "speed", static_cast<double>(options.speed),
                "step_ms", options.step_ms,
                "gravity", options.gravity,
                "jump_height", options.jump_height,
                "air_jumps", options.air_jumps,
                "doors", options.doors
            );
        });
        api["walk"] = walk;

        sol::table input{ lua.create_table() };
        input.set_function("capture_wheel", [module, services](const sol::object& mode) {
            const std::string value{ mode.get_type() == sol::type::string ? mode.as<std::string>() : (mode.get_type() == sol::type::boolean && mode.as<bool>() ? "always" : "off") };
            if (value != "off" && value != "always" && value != "ctrl" && value != "shift" && value != "alt") {
                throw std::runtime_error{ fmt::format("gtproxy.input.capture_wheel: '{}' must be \"off\", \"always\", \"ctrl\", \"shift\" or \"alt\"", value) };
            }
            return services->capture_wheel(module->manifest.id, value);
        });
        api["input"] = input;

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

    // API 3: the map window (overlays, tabs, picking a tile, PNG), ways,
    // and calls between modules.
    static void bind_map(sol::state& lua, sol::table& api, ModuleHost::Loaded* module, Services* services, ModuleApi* self)
    {
        sol::table map{ lua.create_table() };
        // gtproxy.map.overlay(id, { markers = {{x,y,label,color}}, route = {{x,y}...}, areas = {{x1,y1,x2,y2,label,color}} })
        map.set_function("overlay", [module, services](const std::string& id, const sol::object& spec) {
            services->set_overlay(module->manifest.id, id, overlay_from(spec));
        });
        // gtproxy.map.route(x, y, { walk = false, gravity = nil })
        map.set_function("route", [services](sol::this_state state, const double x, const double y, const sol::object& options) {
            bool walk{ false };
            std::optional<bool> gravity;
            if (options.get_type() == sol::type::table) {
                const auto t{ options.as<sol::table>() };
                walk = t.get_or("walk", false);
                if (const sol::object value{ t["gravity"] }; value.get_type() == sol::type::boolean) {
                    gravity = value.as<bool>();
                }
            }
            const auto route{ services->route({ whole_number(x, "gtproxy.map.route: x"), whole_number(y, "gtproxy.map.route: y") }, walk, gravity) };
            sol::state_view view_lua{ state };
            sol::table tiles{ view_lua.create_table() };
            for (std::size_t i{ 0 }; i < route.tiles.size(); ++i) {
                tiles[i + 1] = view_lua.create_table_with("x", route.tiles[i].x, "y", route.tiles[i].y);
            }
            return view_lua.create_table_with(
                "ok", route.ok, "tiles", tiles, "steps", route.tiles.empty() ? 0 : static_cast<int>(route.tiles.size()) - 1, "reason", route.reason
            );
        });
        // gtproxy.map.distances({{x,y}...}) -> steps to each (-1: no way), one search.
        map.set_function("distances", [services](sol::this_state state, const sol::table& targets) {
            sol::state_view view_lua{ state };
            sol::table result{ view_lua.create_table() };
            const auto world{ services->world() };
            const auto steps{ services->distances() };
            for (std::size_t i{ 1 }; i <= targets.size(); ++i) {
                int value{ -1 };
                const sol::object target{ targets[i] };
                if (target.get_type() == sol::type::table && !steps.empty()) {
                    const auto t{ target.as<sol::table>() };
                    const int x{ static_cast<int>(t.get_or("x", -1.0)) };
                    const int y{ static_cast<int>(t.get_or("y", -1.0)) };
                    if (x >= 0 && y >= 0 && x < world.width && y < world.height) {
                        const auto index{ static_cast<std::size_t>(y) * static_cast<std::size_t>(world.width) + static_cast<std::size_t>(x) };
                        value = index < steps.size() ? steps[index] : -1;
                    }
                }
                result[i] = value;
            }
            return result;
        });
        map.set_function("pick", [module, services](const sol::object& label) {
            services->request_pick(module->manifest.id, label.get_type() == sol::type::string ? label.as<std::string>() : std::string{});
        });
        // gtproxy.map.render() -> path of the PNG, or nil and why
        map.set_function("render", [services](sol::this_state state) {
            std::string error;
            const auto path{ services->render_png(error) };
            return std::make_tuple(
                path.empty() ? sol::make_object(state, sol::lua_nil) : sol::make_object(state, path),
                sol::make_object(state, error)
            );
        });
        api["map"] = map;

        sol::table panel{ lua.create_table() };
        // gtproxy.panel.set(id, { title, note, search = {value, placeholder}, columns, rows = {{id, cells, tile, style}}, buttons, inputs })
        panel.set_function("set", [module, services](const std::string& id, const sol::object& spec) {
            if (spec.get_type() != sol::type::table) {
                throw std::runtime_error{ "gtproxy.panel.set: expects a table { title = , columns = , rows = ... }" };
            }
            std::string json;
            to_json(json, spec, 0);
            services->set_panel(module->manifest.id, id, json);
        });
        panel.set_function("remove", [module, services](const std::string& id) {
            services->set_panel(module->manifest.id, id, std::nullopt);
        });
        api["panel"] = panel;

        // Calls between modules: gtproxy.export(name, fn) offers a function,
        // gtproxy.call(module, name, ...) calls another module's (values are
        // copied: true/false, numbers, text and tables of them).
        api.set_function("export", [module](const std::string& name, const sol::object& fn) {
            if (fn.get_type() != sol::type::function) {
                throw std::runtime_error{ "gtproxy.export: the second argument must be a function" };
            }
            module->exports[name] = fn.as<sol::protected_function>();
        });
        api.set_function("call", [self](sol::this_state state, const std::string& target, const std::string& name, const sol::variadic_args& args) {
            return self->caller_(state, target, name, args);
        });
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
        // Wall clock: seconds since 1970 (UTC), and the local date and time
        // as text, "2026-11-15 18:04:05" (modules have no os library).
        api.set_function("time", [] {
            return static_cast<lua_Integer>(std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        });
        api.set_function("date", [](const sol::object& when) {
            const auto seconds{ when.get_type() == sol::type::number
                ? static_cast<std::time_t>(when.as<double>())
                : std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()) };
            std::tm local{};
#ifdef _WIN32
            localtime_s(&local, &seconds);
#else
            localtime_r(&seconds, &local);
#endif
            return fmt::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
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

void ModuleHost::refresh_settings(const Resolved& resolved)
{
    settings_ = resolved.settings;
    for (auto& module : loaded_) {
        if (const auto it{ settings_.find(module->manifest.id) }; it != settings_.end()) {
            module->settings = it->second;
        }
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
    module->engine->register_binding(std::make_unique<ModuleApi>(
        *module,
        services_,
        [this](const std::string_view name) {
            return (reserved_ && reserved_(name)) || find_command(name) != nullptr;
        },
        [this](sol::this_state state, const std::string& target, const std::string& name, const sol::variadic_args& args) {
            return call_export(state, target, name, args);
        }
    ));

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

sol::object ModuleHost::call_export(sol::this_state state, const std::string& target, const std::string& name, const sol::variadic_args& args)
{
    const auto it{ std::ranges::find_if(loaded_, [&](const std::unique_ptr<Loaded>& module) { return module->manifest.id == target; }) };
    if (it == loaded_.end()) {
        throw std::runtime_error{ fmt::format("gtproxy.call: module '{}' is not loaded", target) };
    }
    auto& module{ **it };
    const auto fn{ module.exports.find(name) };
    if (fn == module.exports.end()) {
        throw std::runtime_error{ fmt::format("gtproxy.call: module '{}' offers no '{}'", target, name) };
    }

    sol::state& to{ module.engine->state() };
    std::vector<sol::object> copied;
    for (const auto& arg : args) {
        copied.push_back(copy_value(arg, to, 0));
    }
    const Watchdog watchdog{ to.lua_state() };
    sol::protected_function_result result{ fn->second(sol::as_args(copied)) };
    if (!result.valid()) {
        const sol::error error{ result };
        throw std::runtime_error{ fmt::format("gtproxy.call {}.{}: {}", target, name, error.what()) };
    }
    if (result.return_count() == 0) {
        return sol::make_object(state, sol::lua_nil);
    }
    return copy_value(result.get<sol::object>(), sol::state_view{ state }, 0);
}

ModuleHost::Reply ModuleHost::emit_to(const std::string_view id, const std::string_view event, const Fields& fields)
{
    Reply reply{};
    const auto it{ std::ranges::find_if(loaded_, [&](const std::unique_ptr<Loaded>& module) { return module->manifest.id == id; }) };
    if (it == loaded_.end()) {
        return reply;
    }
    auto& module{ **it };
    const auto handlers{ module.handlers.find(std::string{ event }) };
    if (handlers == module.handlers.end()) {
        return reply;
    }
    for (const auto& handler : handlers->second) {
        sol::table table{ module.engine->state().create_table() };
        if (fields) {
            fields(table);
        }
        reply.ran = true;
        const auto result{ call_guarded(module, handler, fmt::format("{} handler", event), table) };
        if (!result) {
            reply.ok = false;
            reply.message = "the module failed (see the proxy log)";
            return reply;
        }
        if (result->return_count() == 0) {
            continue;
        }
        const sol::object value{ result->get<sol::object>() };
        if (value.get_type() == sol::type::string) {
            reply.message = value.as<std::string>();
            return reply;
        }
        if (value.get_type() == sol::type::table) {
            const auto t{ value.as<sol::table>() };
            reply.ok = t.get_or("ok", true);
            reply.message = t.get_or<std::string>("message", "");
            return reply;
        }
    }
    return reply;
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
