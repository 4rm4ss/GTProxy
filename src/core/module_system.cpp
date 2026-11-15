#include "module_system.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

#include <fmt/format.h>
#include <glaze/glaze.hpp>
#include <magic_enum/magic_enum.hpp>
#include <spdlog/spdlog.h>

#include "features.hpp"
#include "../item/item_database.hpp"
#include "../map/grid.hpp"
#include "../map/reachability.hpp"
#include "../packet/game/dialog.hpp"
#include "../packet/game/player.hpp"
#include "../packet/game/world.hpp"
#include "../packet/message/dialog.hpp"
#include "../world/world.hpp"

namespace core {
namespace {
template <packet::PacketId Id>
const event::TypedPacketEvent<Id>* as(const event::Event& event, const event::Direction direction)
{
    const auto evt{ dynamic_cast<const event::TypedPacketEvent<Id>*>(&event) };
    if (!evt || evt->direction != direction) {
        return nullptr;
    }
    return evt;
}

modules::ItemView item_view(const item::ItemInfo& info)
{
    modules::ItemView view{};
    view.id = info.item_id;
    view.name = info.item_name;
    view.type = std::string{ magic_enum::enum_name(info.item_type) };
    view.rarity = info.rarity;
    view.max_amount = info.max_amount;
    view.untradeable = info.flags.untradeable != 0;
    return view;
}

modules::PlayerView player_view(const glm::vec2 position, const map::Body& body)
{
    const auto tile{ map::body_tile(position, body) };
    return modules::PlayerView{ position.x, position.y, tile.x, tile.y };
}
}

ModuleSystem::ModuleSystem(
    const std::filesystem::path& app_dir,
    event::Dispatcher& dispatcher,
    handlers::SessionHandler& session,
    handlers::InventoryHandler& inventory,
    handlers::MapHandler& map,
    command::CommandHandler& commands
)
    : modules_dir_{ app_dir / "modules" }
    , data_dir_{ app_dir / "data" }
    , config_{ app_dir / "config" }
    , dispatcher_{ dispatcher }
    , session_{ session }
    , inventory_{ inventory }
    , map_{ map }
    , commands_{ commands }
    , registry_{ commands.registry() }
    , host_{ *this }
{
    // Modules cannot take the proxy's own commands.
    host_.set_reserved_names([this](const std::string_view name) {
        return name == "cfg" || name == "help"
            || (registry_.get(name) != nullptr && !chat_commands_.contains(std::string{ name }));
    });
    host_.set_on_change([this] { sync_commands(); });
    host_.set_data_dir(data_dir_);
    import_old_settings(app_dir);

    spdlog::info("Modules: {} | settings: {} | data: {}", modules_dir_.string(), config_.dir().string(), data_dir_.string());

    auto found{ modules::discover(modules_dir_) };
    for (const auto& problem : found.problems) {
        spdlog::error("Module skipped: {}", problem);
    }
    if (std::error_code ec; !std::filesystem::is_directory(modules_dir_, ec)) {
        spdlog::info("No modules/ folder next to the proxy: running without modules (the proxy only forwards packets)");
    }
    host_.set_available(std::move(found.modules));

    const auto modules_file{ config_.read_modules_file() };
    if (modules_file) {
        profile_ = modules_file->profile;
    }
    else {
        spdlog::error("Config error: {}: {}", modules_file.error().file, modules_file.error().reason);
    }

    // Everything the modules hear about is wired before they load.
    listen_packets();

    map_.set_click_hook([this](const map::MouseButton button, const map::TilePos tile) {
        in_click_ = true;
        card_.reset();
        const bool in_world{ session_.in_world() };
        const auto ran{
            button == map::MouseButton::Right
                ? host_.right_click(tile, in_world)
                : host_.emit("left_click", [&](sol::table& event) {
                    event["x"] = tile.x;
                    event["y"] = tile.y;
                    event["in_world"] = in_world;
                }).ran
        };
        in_click_ = false;
        return handlers::MapHandler::ClickAnswer{ ran > 0, std::exchange(card_, std::nullopt) };
    });

    map_.set_walk_end_hook([this](const std::string& message, const bool notify) {
        host_.emit("walk_end", [&](sol::table& event) {
            event["message"] = message;
            event["notify"] = notify;
        });
    });

    inventory_.set_on_change([this](const handlers::InventoryHandler::Change& change) {
        host_.emit("inventory", [&](sol::table& event) {
            event["full"] = change.full;
            if (!change.full) {
                event["item"] = change.item_id;
                event["removed"] = change.removed;
                event["added"] = change.added;
                event["count"] = inventory_.count_of(change.item_id);
            }
        });
    });

    commands_.set_dialog_handler([this](const packet::message::DialogReturn& dialog, const bool frame) {
        host_.emit("dialog", [&](sol::table& event) {
            event["name"] = dialog.dialog_name;
            event["button"] = dialog.button_clicked;
            event["frame"] = frame;
            sol::state_view lua{ event.lua_state() };
            sol::table values{ lua.create_table() };
            for (const auto& [key, parts] : dialog.text_parse.get_data()) {
                if (key != "action" && key != "dialog_name" && key != "buttonClicked") {
                    values[key] = dialog.field(key);
                }
            }
            event["values"] = values;
        });
    });

    commands_.menu().set_sources(
        [this] {
            std::vector<command::Menu::Section> sections;
            for (const auto& section : host_.menu_sections()) {
                sections.push_back({ section.title, section.icon, section.button, section.label, section.order });
            }
            std::ranges::stable_sort(sections, {}, &command::Menu::Section::order);
            return sections;
        },
        [this] { return host_.menu_status(); }
    );

    if (!apply_profile(profile_, "start")) {
        // Nothing that worked before to fall back on: the modules' own defaults.
        modules::Resolved defaults{};
        defaults.profile = profile_;
        for (const auto& manifest : host_.available()) {
            defaults.settings[manifest.id] = manifest.settings;
            bool on{ true };
            if (modules_file) {
                if (const auto it{ modules_file->modules.find(manifest.id) }; it != modules_file->modules.end()) {
                    on = it->second.enabled;
                }
            }
            defaults.enabled[manifest.id] = on;
        }
        enabled_ = defaults.enabled;
        host_.apply(defaults);
        spdlog::warn("Modules use their default settings until the config is fixed ('cfg reload' in this console)");
        log_settings();
    }
}

ModuleSystem::~ModuleSystem()
{
    handles_.clear();
    map_.set_click_hook({});
    map_.set_walk_end_hook({});
    inventory_.set_on_change({});
    commands_.set_dialog_handler({});
    commands_.menu().set_sources({}, {});
    host_.set_on_change({});
    for (const auto& name : chat_commands_) {
        registry_.remove(name);
    }
}

void ModuleSystem::listen_packets()
{
    const auto listen = [this](const packet::PacketId id, const int8_t priority, std::function<void(const event::Event&)> handler) {
        const auto type{ event::packet_event_type(id) };
        handles_.emplace_back(dispatcher_, type, dispatcher_.append_listener(type, std::move(handler), priority));
    };

    // Last: the proxy's own state (world, players, session) is up to date,
    // and a call a module changes is sent to the game in place of the
    // server's.
    listen(packet::PacketId::OnSpawn, event::Priority::Lowest, [this](const event::Event& e) { hook_call<packet::game::OnSpawn>(e); });
    listen(packet::PacketId::OnRemove, event::Priority::Lowest, [this](const event::Event& e) { hook_call<packet::game::OnRemove>(e); });
    listen(packet::PacketId::OnNameChanged, event::Priority::Lowest, [this](const event::Event& e) { hook_call<packet::game::OnNameChanged>(e); });
    listen(packet::PacketId::OnSetPos, event::Priority::Lowest, [this](const event::Event& e) { hook_call<packet::game::OnSetPos>(e); });
    // Before scripts/ see it: a module may answer the dialog itself (fast drop).
    listen(packet::PacketId::OnDialogRequest, event::Priority::FairlyHigh, [this](const event::Event& e) { hook_call<packet::game::OnDialogRequest>(e); });

    listen(packet::PacketId::OnRequestWorldSelectMenu, event::Priority::Lowest, [this](const event::Event& e) {
        if (as<packet::PacketId::OnRequestWorldSelectMenu>(e, event::Direction::ClientBound)) {
            host_.emit("world_menu");
            hook_call<packet::game::OnRequestWorldSelectMenu>(e);
        }
    });

    listen(packet::PacketId::SendMapData, event::Priority::Lowest, [this](const event::Event& e) {
        if (as<packet::PacketId::SendMapData>(e, event::Direction::ClientBound)) {
            host_.emit("world_enter", [this](sol::table& event) {
                event["name"] = session_.current_world();
                event["previous"] = session_.previous_world();
            });
        }
    });

    // Before the map handler and the server see a punch / build.
    listen(packet::PacketId::TileChangeRequest, event::Priority::FairlyHigh, [this](const event::Event& e) { on_tile_change(e); });
    // Before the world handler removes a collected object.
    listen(packet::PacketId::ItemChangeObject, event::Priority::Normal, [this](const event::Event& e) { on_object(e); });

    for (const auto type : { event::Type::ClientDisconnect, event::Type::ServerDisconnect }) {
        handles_.emplace_back(dispatcher_, type, dispatcher_.append_listener(type, [this](const event::Event&) { host_.emit("disconnect"); }));
    }
}

template <typename Packet>
void ModuleSystem::hook_call(const event::Event& event)
{
    const auto evt{ as<Packet::ID>(event, event::Direction::ClientBound) };
    const auto pkt{ evt ? evt->template get<Packet>() : nullptr };
    if (!pkt || pkt->variant.size() == 0 || evt->canceled) {
        return;
    }

    modules::ModuleHost::Call call{};
    call.name = pkt->variant.template get<std::string>(0);
    call.net_id = static_cast<std::int32_t>(pkt->game_packet.net_id);
    const auto& args{ pkt->variant.get_variants() };
    call.args.assign(args.begin() + 1, args.end());

    const auto outcome{ host_.hook_call(call) };
    if (outcome.cancelled) {
        spdlog::debug("{} kept from the game by a module", call.name);
        evt->cancel();
        return;
    }
    if (!outcome.replaced) {
        return;
    }

    packet::PacketVariant variant{};
    variant.add(call.name);
    for (const auto& arg : call.args) {
        variant.add<packet::variant>(arg);
    }
    auto header{ pkt->game_packet };
    header.net_id = static_cast<std::uint32_t>(call.net_id);
    evt->cancel();
    std::ignore = session_.send_to_client(packet::VariantPayload{ header, variant });
}

void ModuleSystem::on_tile_change(const event::Event& event)
{
    const auto evt{ as<packet::PacketId::TileChangeRequest>(event, event::Direction::ServerBound) };
    const auto pkt{ evt ? evt->get<packet::game::TileChangeRequest>() : nullptr };
    if (!pkt) {
        return;
    }

    const auto outcome{ host_.emit("tile_click", [&](sol::table& table) {
        table["x"] = pkt->int_x;
        table["y"] = pkt->int_y;
        table["item"] = pkt->item_id;
    }) };
    if (outcome.cancelled) {
        evt->cancel();
    }
}

void ModuleSystem::on_object(const event::Event& event)
{
    const auto evt{ as<packet::PacketId::ItemChangeObject>(event, event::Direction::ClientBound) };
    const auto pkt{ evt ? evt->get<packet::game::ItemChangeObject>() : nullptr };
    if (!pkt) {
        return;
    }

    // change: -1 a new object (something was dropped), -3 an object
    // changed, a netID: that player collected it.
    host_.emit("object", [&](sol::table& table) {
        table["change"] = static_cast<std::int32_t>(pkt->game_packet.net_id);
        table["item"] = pkt->item_id;
        table["amount"] = pkt->amount;
        table["object_id"] = pkt->item_net_id;
        table["by_me"] = session_.local_net_id() >= 0 && static_cast<std::int32_t>(pkt->game_packet.net_id) == session_.local_net_id();
    });
}

void ModuleSystem::update()
{
    host_.update();
}

bool ModuleSystem::run_console(const std::vector<std::string>& words)
{
    if (words.empty()) {
        return false;
    }
    if (words[0] == "cfg") {
        cfg(words);
        return true;
    }
    return host_.run_command(words[0], { words.begin() + 1, words.end() }, modules::Source::Console);
}

std::vector<std::string> ModuleSystem::console_help() const
{
    std::vector<std::string> lines{
        "cfg list - the profiles in config/profiles",
        "cfg exec <profile> - apply config/profiles/<profile>.json",
        "cfg reload - read config/ again for the active profile",
    };
    for (const auto& command : host_.commands()) {
        lines.push_back(fmt::format("{} - {} (module {})", command.name, command.description, command.module));
    }
    return lines;
}

void ModuleSystem::cfg(const std::vector<std::string>& words)
{
    if (words.size() == 2 && words[1] == "list") {
        cfg_list();
        return;
    }
    if (words.size() == 3 && words[1] == "exec") {
        apply_profile(words[2], "cfg exec");
        return;
    }
    if (words.size() == 2 && words[1] == "reload") {
        apply_profile(profile_, "cfg reload");
        return;
    }
    spdlog::info("Usage: cfg list | cfg exec <profile> | cfg reload (active profile: {})", profile_);
}

void ModuleSystem::cfg_list() const
{
    const auto names{ config_.profiles() };
    if (names.empty()) {
        spdlog::info("No profiles in {} (active: {})", (config_.dir() / "profiles").string(), profile_);
        return;
    }

    spdlog::info("Profiles in config/profiles (* = active):");
    for (const auto& name : names) {
        const auto description{ config_.profile_description(name) };
        spdlog::info(
            "  {} {}{}",
            name == profile_ ? '*' : ' ',
            name,
            description ? fmt::format(" - {}", *description) : std::string{}
        );
    }
}

bool ModuleSystem::apply_profile(const std::string& profile, const std::string_view why)
{
    const auto resolved{ config_.resolve(profile, host_.defaults()) };
    if (!resolved) {
        for (const auto& error : resolved.error()) {
            spdlog::error("Config error: {}: {}", error.file, error.reason);
        }
        if (why != "start") {
            spdlog::warn("{} '{}' failed: the settings in force are kept (profile '{}')", why, profile, profile_);
        }
        return false;
    }

    for (const auto& warning : resolved->warnings) {
        spdlog::warn("Config: {}", warning);
    }

    enabled_ = resolved->enabled;
    host_.apply(*resolved);
    profile_ = profile;
    spdlog::info("Profile '{}' applied ({})", profile_, why);
    log_settings();
    return true;
}

void ModuleSystem::log_settings() const
{
    for (const auto& manifest : host_.available()) {
        const auto it{ enabled_.find(manifest.id) };
        const bool on{ it == enabled_.end() || it->second };
        const char* state{ host_.loaded(manifest.id) ? "on" : (on ? "failed to load" : "off") };

        std::string values;
        if (const auto settings{ host_.settings(manifest.id) }) {
            for (const auto& [key, value] : *settings) {
                values += fmt::format("{}{}={}", values.empty() ? "" : ", ", key, modules::to_string(value));
            }
        }
        spdlog::info("Module {} {}: {} | {}", manifest.id, manifest.version, state, values.empty() ? "no settings" : values);
    }
}

void ModuleSystem::sync_commands()
{
    std::set<std::string> wanted;
    for (const auto& command : host_.commands()) {
        wanted.insert(command.name);
    }

    for (auto it{ chat_commands_.begin() }; it != chat_commands_.end();) {
        if (!wanted.contains(*it)) {
            registry_.remove(*it);
            it = chat_commands_.erase(it);
        }
        else {
            ++it;
        }
    }

    for (const auto& command : host_.commands()) {
        if (chat_commands_.contains(command.name)) {
            continue;
        }
        if (registry_.get(command.name)) {
            spdlog::error("[{}] /{} is a command of the proxy; the module's one is not added", command.module, command.name);
            continue;
        }
        registry_.add(command.name, command.description, [this, name = command.name](const command::Context& ctx) {
            host_.run_command(name, ctx.args, modules::Source::Game);
            return command::Result::Success;
        });
        chat_commands_.insert(command.name);
    }
}

void ModuleSystem::import_old_settings(const std::filesystem::path& app_dir) const
{
    const auto target{ data_dir_ / "drop.json" };
    std::error_code ec;
    if (std::filesystem::exists(target, ec)) {
        return;
    }

    // Earlier versions kept the item settings in the folder the proxy was
    // started from.
    std::filesystem::path source;
    for (const auto& dir : { std::filesystem::current_path(ec), app_dir }) {
        if (std::filesystem::exists(dir / "gtproxy-settings.txt", ec)) {
            source = dir / "gtproxy-settings.txt";
            break;
        }
    }
    if (source.empty()) {
        return;
    }

    std::ifstream in{ source };
    modules::Settings values;
    for (std::string line; std::getline(in, line);) {
        const auto eq{ line.find('=') };
        if (eq == std::string::npos || line.starts_with('#')) {
            continue;
        }
        const std::string key{ line.substr(0, eq) };
        std::string value{ line.substr(eq + 1) };
        if (!value.empty() && value.back() == '\r') {
            value.pop_back();
        }

        if (key == "fast_drop" || key == "fast_trash") {
            values[key] = value == "1";
        }
        else if (key == "drop_gap_ms") {
            int ms{};
            if (std::from_chars(value.data(), value.data() + value.size(), ms).ec == std::errc{}) {
                values[key] = static_cast<double>(ms);
            }
        }
        else if (key == "keep") {
            values[key] = value;
        }
    }

    std::string json;
    if (values.empty() || glz::write<glz::opts{ .prettify = true }>(values, json)) {
        return;
    }
    std::filesystem::create_directories(data_dir_, ec);
    std::ofstream out{ target, std::ios::binary | std::ios::trunc };
    out << json;
    if (out) {
        spdlog::info("Item settings of the earlier version ({}) moved to {}", source.string(), target.string());
    }
}

modules::WorldView ModuleSystem::world() const
{
    modules::WorldView view{};
    view.connected = session_.connected();
    view.in_world = session_.in_world();
    view.previous = session_.previous_world();
    view.recent = { session_.recent_worlds().begin(), session_.recent_worlds().end() };
    view.net_id = session_.local_net_id();
    view.real_name = session_.real_name();
    if (!view.in_world) {
        return view;
    }

    const auto& world{ world::World::instance() };
    view.name = session_.current_world();
    view.width = world.get_tile_map().get_size().x;
    view.height = world.get_tile_map().get_size().y;

    // Your position: the last one the game sent, else where the server put you.
    const auto me{ world.get_local_player().lock() };
    const map::Body body{ me ? map::Body::from_colrect(me->collision()) : map::Body{} };
    if (const auto position{ session_.local_position() }) {
        view.me = player_view(*position, body);
    }
    else if (me) {
        view.me = player_view(me->position(), body);
    }
    return view;
}

modules::ReachView ModuleSystem::reach(const map::TilePos tile)
{
    const auto report{ map_.check_reach(tile) };
    modules::ReachView view{};
    view.x = tile.x;
    view.y = tile.y;
    view.status = map::code(report.result.status);
    view.headline = map::headline(report.result.status);
    view.reason = map::reason(report.result.status);
    view.message = report.message();
    view.missing = report.missing;
    view.start = report.result.start;
    view.steps = report.result.steps;
    view.steps_word = map::steps_word(report.result.steps);
    view.tile_text = report.tile_text;
    view.summary = report.summary();
    return view;
}

bool ModuleSystem::show(const map::Card& card)
{
    if (!in_click_) {
        return false;
    }
    card_ = card;
    return true;
}

void ModuleSystem::reply(const modules::Source source, const std::string& module_id, const std::string& text)
{
    if (source == modules::Source::Game) {
        session_.message(text);
        return;
    }
    spdlog::info("[{}] {}", module_id, features::strip_colors(text));
}

std::vector<modules::PlayerInfo> ModuleSystem::players() const
{
    std::vector<modules::PlayerInfo> list;
    if (!session_.in_world()) {
        return list;
    }
    for (const auto& [net_id, player] : world::World::instance().get_players()) {
        if (!player) {
            continue;
        }
        modules::PlayerInfo info{};
        info.net_id = player->net_id();
        info.user_id = player->user_id();
        info.name = player->name();
        info.country = player->country_code();
        info.invisible = player->invisible() != 0;
        info.local = player->is_local();
        info.position = player_view(player->position(), map::Body::from_colrect(player->collision()));
        list.push_back(std::move(info));
    }
    std::ranges::sort(list, {}, &modules::PlayerInfo::net_id);
    return list;
}

std::vector<modules::ObjectInfo> ModuleSystem::objects() const
{
    std::vector<modules::ObjectInfo> list;
    if (!session_.in_world()) {
        return list;
    }
    for (const auto& object : world::World::instance().get_object_map().get_objects()) {
        list.push_back({ object.object_id, object.item_id, object.amount, object.pos.x, object.pos.y });
    }
    return list;
}

std::optional<modules::TileInfo> ModuleSystem::tile(const map::TilePos tile) const
{
    const auto* data{ session_.in_world() ? world::World::instance().get_tile(tile.x, tile.y) : nullptr };
    if (!data) {
        return std::nullopt;
    }
    modules::TileInfo info{};
    info.foreground = data->foreground;
    info.background = data->background;
    info.flags = static_cast<std::uint16_t>(data->flag);
    info.cell = std::string{ map::describe(map::classify(*data)) };
    info.text = map_.inspect(tile).message;
    return info;
}

modules::InventoryView ModuleSystem::inventory() const
{
    modules::InventoryView view{};
    view.known = inventory_.known();
    view.slots = inventory_.slots();
    for (const auto& [id, item] : inventory_.items()) {
        view.items.push_back({ item.id, item.count, item.flags });
    }
    return view;
}

bool ModuleSystem::items_loaded() const
{
    return !item::ItemDatabase::instance().empty();
}

std::optional<modules::ItemView> ModuleSystem::item(const std::uint32_t id) const
{
    if (const auto* info{ item::ItemDatabase::instance().get_item(id) }) {
        return item_view(*info);
    }
    return std::nullopt;
}

std::vector<modules::ItemView> ModuleSystem::find_items(const std::string_view query, const std::size_t limit) const
{
    std::vector<modules::ItemView> list;
    for (const auto* info : item::ItemDatabase::instance().find(query, limit)) {
        list.push_back(item_view(*info));
    }
    return list;
}

char ModuleSystem::prefix() const
{
    return registry_.prefix();
}

void ModuleSystem::game_message(const std::string& text)
{
    session_.message(text);
}

void ModuleSystem::game_dialog(const std::string& markup)
{
    session_.show_dialog(markup);
}

bool ModuleSystem::game_call(const modules::CallOut& call)
{
    packet::PacketVariant variant{};
    variant.add(call.name);
    for (const auto& arg : call.args) {
        variant.add<packet::variant>(arg);
    }
    packet::VariantPayload payload{ variant };
    payload.game_packet.net_id = static_cast<std::uint32_t>(call.net_id);
    payload.game_packet.int_data = call.delay;
    return session_.send_to_client(payload);
}

bool ModuleSystem::server_text(const std::string& text, const bool game_message)
{
    return session_.send_text_to_server(text, game_message ? packet::NET_MESSAGE_GAME_MESSAGE : packet::NET_MESSAGE_GENERIC_TEXT);
}

modules::WalkView ModuleSystem::walk_to(const map::TilePos tile)
{
    const auto result{ map_.walk_to(tile) };
    return { result.ok, result.message, result.tile };
}

modules::WalkView ModuleSystem::walk_stop()
{
    const auto result{ map_.stop() };
    return { result.ok, result.message, result.tile };
}

bool ModuleSystem::walking() const
{
    return map_.walking();
}
}
