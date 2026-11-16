#include "module_system.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

#include <fmt/format.h>
#include <glaze/glaze.hpp>
#include <magic_enum/magic_enum.hpp>
#include <spdlog/spdlog.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "features.hpp"
#include "../modules/json.hpp"
#include "../modules/packet_view.hpp"
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
    command::CommandHandler& commands,
    handlers::PlayerMods& mods
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
    , mods_{ mods }
    , host_{ *this }
{
    // Modules cannot take the proxy's own commands.
    host_.set_reserved_names([this](const std::string_view name) {
        return name == "cfg" || name == "help"
            || (registry_.get(name) != nullptr && !chat_commands_.contains(std::string{ name }));
    });
    host_.set_on_change([this] {
        sync_commands();
        update_wheel();
        // Tabs and marks of modules no longer loaded go away.
        const auto gone = [&](const auto& entry) { return !host_.loaded(entry.first.substr(0, entry.first.find('/'))); };
        std::erase_if(panels_, gone);
        std::erase_if(overlays_, gone);
        if (!pick_owner_.empty() && !host_.loaded(pick_owner_)) {
            pick_owner_.clear();
            map_.set_pick({});
        }
        publish_module_view();
    });
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

        // A module asked for a tile (gtproxy.map.pick): the left click is its.
        if (button == map::MouseButton::Left && !pick_owner_.empty()) {
            const auto owner{ std::exchange(pick_owner_, std::string{}) };
            map_.set_pick({});
            const auto reply{ host_.emit_to(owner, "pick", [&](sol::table& event) {
                event["x"] = tile.x;
                event["y"] = tile.y;
                event["in_world"] = in_world;
            }) };
            in_click_ = false;
            auto card{ std::exchange(card_, std::nullopt) };
            if (!card && !reply.message.empty()) {
                card = map::Card{ reply.message, {}, reply.ok ? "ok" : "bad", {}, true, true, tile };
            }
            return handlers::MapHandler::ClickAnswer{ reply.ran, card };
        }

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

    map_.set_panel_hook([this](const std::map<std::string, std::string>& params) { return answer_panel(params); });

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
    map_.set_panel_hook({});
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
            forget_module_view(false);
            host_.emit("world_menu");
            hook_call<packet::game::OnRequestWorldSelectMenu>(e);
        }
    });

    listen(packet::PacketId::SendMapData, event::Priority::Lowest, [this](const event::Event& e) {
        if (as<packet::PacketId::SendMapData>(e, event::Direction::ClientBound)) {
            forget_module_view(false);
            host_.emit("world_enter", [this](sol::table& event) {
                event["name"] = session_.current_world();
                event["previous"] = session_.previous_world();
            });
        }
    });

    // Before the map handler and the server see a punch / build.
    listen(packet::PacketId::TileChangeRequest, event::Priority::FairlyHigh, [this](const event::Event& e) { on_tile_change(e); });

    // Blocks the server changed: what was there before (first), then the
    // "tile" event once the world has it (last).
    const auto tile_before = [this](const int x, const int y) {
        const auto* tile{ world::World::instance().get_tile(x, y) };
        tile_before_ = TileBefore{ x, y, tile ? tile->foreground : std::uint16_t{ 0 }, tile ? tile->background : std::uint16_t{ 0 } };
    };
    const auto tile_after = [this](const int x, const int y) {
        if (!tile_before_ || tile_before_->x != x || tile_before_->y != y) {
            return;
        }
        const auto before{ *std::exchange(tile_before_, std::nullopt) };
        const auto* tile{ world::World::instance().get_tile(x, y) };
        if (!tile) {
            return;
        }
        const bool fg_changed{ tile->foreground != before.fg };
        const bool bg_changed{ tile->background != before.bg };
        std::string cause{ "changed" };
        if (fg_changed ? tile->foreground == 0 : (bg_changed && tile->background == 0)) {
            cause = "broken";
        }
        else if (fg_changed ? before.fg == 0 : (bg_changed && before.bg == 0)) {
            cause = "placed";
        }
        else if (!fg_changed && !bg_changed) {
            cause = "updated";
        }
        host_.emit("tile", [&](sol::table& table) {
            table["x"] = x;
            table["y"] = y;
            table["fg"] = tile->foreground;
            table["bg"] = tile->background;
            table["old_fg"] = before.fg;
            table["old_bg"] = before.bg;
            table["cause"] = cause;
        });
    };
    listen(packet::PacketId::TileChangeRequest, event::Priority::Highest, [this, tile_before](const event::Event& e) {
        if (const auto evt{ as<packet::PacketId::TileChangeRequest>(e, event::Direction::ClientBound) }) {
            if (const auto pkt{ evt->get<packet::game::TileChangeRequest>() }) {
                tile_before(pkt->int_x, pkt->int_y);
            }
        }
    });
    listen(packet::PacketId::TileChangeRequest, event::Priority::Lowest, [tile_after](const event::Event& e) {
        if (const auto evt{ as<packet::PacketId::TileChangeRequest>(e, event::Direction::ClientBound) }) {
            if (const auto pkt{ evt->get<packet::game::TileChangeRequest>() }) {
                tile_after(pkt->int_x, pkt->int_y);
            }
        }
    });
    listen(packet::PacketId::SendTileUpdateData, event::Priority::Highest, [tile_before](const event::Event& e) {
        if (const auto evt{ as<packet::PacketId::SendTileUpdateData>(e, event::Direction::ClientBound) }) {
            if (const auto pkt{ evt->get<packet::game::SendTileUpdateData>() }) {
                tile_before(pkt->game_packet.int_x, pkt->game_packet.int_y);
            }
        }
    });
    listen(packet::PacketId::SendTileUpdateData, event::Priority::Lowest, [tile_after](const event::Event& e) {
        if (const auto evt{ as<packet::PacketId::SendTileUpdateData>(e, event::Direction::ClientBound) }) {
            if (const auto pkt{ evt->get<packet::game::SendTileUpdateData>() }) {
                tile_after(pkt->game_packet.int_x, pkt->game_packet.int_y);
            }
        }
    });

    // Dropped objects: the object as it was (first), the event (last).
    listen(packet::PacketId::ItemChangeObject, event::Priority::Highest, [this](const event::Event& e) {
        object_before_.reset();
        const auto evt{ as<packet::PacketId::ItemChangeObject>(e, event::Direction::ClientBound) };
        const auto pkt{ evt ? evt->get<packet::game::ItemChangeObject>() : nullptr };
        if (!pkt || static_cast<std::int32_t>(pkt->game_packet.net_id) == -1) {
            return;
        }
        for (const auto& object : world::World::instance().get_object_map().get_objects()) {
            if (object.object_id == pkt->item_net_id) {
                object_before_ = ObjectBefore{ object.object_id, object.item_id, object.amount, object.pos.x, object.pos.y };
                break;
            }
        }
    });
    listen(packet::PacketId::ItemChangeObject, event::Priority::Lowest, [this](const event::Event& e) { on_object(e); });

    // Players coming and going (after the world has them / before it forgets them).
    listen(packet::PacketId::OnSpawn, event::Priority::Lowest, [this](const event::Event& e) {
        const auto evt{ as<packet::PacketId::OnSpawn>(e, event::Direction::ClientBound) };
        const auto pkt{ evt ? evt->get<packet::game::OnSpawn>() : nullptr };
        if (!pkt || evt->canceled) {
            return;
        }
        host_.emit("player_join", [&](sol::table& table) {
            table["net_id"] = pkt->net_id;
            table["user_id"] = pkt->user_id;
            table["name"] = pkt->name;
            table["clean"] = features::strip_colors(pkt->name);
            table["country"] = pkt->country_code;
            table["is_local"] = pkt->type == "local";
            table["invisible"] = pkt->invisible != 0;
        });
    });
    listen(packet::PacketId::OnRemove, event::Priority::Highest, [this](const event::Event& e) {
        player_before_.reset();
        const auto evt{ as<packet::PacketId::OnRemove>(e, event::Direction::ClientBound) };
        const auto pkt{ evt ? evt->get<packet::game::OnRemove>() : nullptr };
        if (!pkt) {
            return;
        }
        PlayerBefore before{ pkt->net_id, pkt->player_id, {} };
        if (const auto player{ world::World::instance().get_player(pkt->net_id).lock() }) {
            before.user_id = player->user_id();
            before.name = player->name();
        }
        player_before_ = before;
    });
    listen(packet::PacketId::OnRemove, event::Priority::Lowest, [this](const event::Event& e) {
        if (!as<packet::PacketId::OnRemove>(e, event::Direction::ClientBound) || !player_before_) {
            return;
        }
        const auto before{ *std::exchange(player_before_, std::nullopt) };
        host_.emit("player_leave", [&](sol::table& table) {
            table["net_id"] = before.net_id;
            table["user_id"] = before.user_id;
            table["name"] = before.name;
            table["clean"] = features::strip_colors(before.name);
        });
    });

    handles_.emplace_back(dispatcher_, event::Type::ClientBoundPacket, dispatcher_.append_listener(
        event::Type::ClientBoundPacket, [this](const event::Event& e) { on_raw_client_packet(e); }, event::Priority::Highest
    ));

    // (API 5) Every packet, both ways, before it is passed on.
    handles_.emplace_back(dispatcher_, event::Type::ClientBoundPacket, dispatcher_.append_listener(
        event::Type::ClientBoundPacket, [this](const event::Event& e) { on_raw_packet(e, true); }, event::Priority::Highest
    ));
    handles_.emplace_back(dispatcher_, event::Type::ServerBoundPacket, dispatcher_.append_listener(
        event::Type::ServerBoundPacket, [this](const event::Event& e) { on_raw_packet(e, false); }, event::Priority::Highest
    ));

    for (const auto type : { event::Type::ClientDisconnect, event::Type::ServerDisconnect }) {
        handles_.emplace_back(dispatcher_, type, dispatcher_.append_listener(type, [this](const event::Event&) {
            forget_module_view(false);
            host_.emit("disconnect");
        }));
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
    // changed, a netID: that player collected it, else it went away.
    const auto change{ static_cast<std::int32_t>(pkt->game_packet.net_id) };
    const auto before{ std::exchange(object_before_, std::nullopt) };
    const auto& objects{ world::World::instance().get_object_map().get_objects() };

    std::string kind;
    std::uint32_t object_id{ pkt->item_net_id };
    std::uint16_t item{ pkt->item_id };
    int amount{ pkt->amount };
    int old_amount{ before ? before->amount : -1 };
    float x{ static_cast<float>(pkt->pos_x) };
    float y{ static_cast<float>(pkt->pos_y) };
    std::string by_name;
    if (change == -1) {
        kind = "dropped";
        if (!objects.empty()) {
            const auto& added{ objects.back() };
            object_id = added.object_id;
            x = added.pos.x;
            y = added.pos.y;
        }
    }
    else if (change == -3) {
        kind = "changed";
    }
    else {
        kind = change >= 0 ? "collected" : "removed";
        if (before) {
            item = before->item;
            amount = before->amount;
            x = before->x;
            y = before->y;
        }
        if (change >= 0) {
            if (const auto player{ world::World::instance().get_player(change).lock() }) {
                by_name = player->name();
            }
        }
    }

    host_.emit("object", [&](sol::table& table) {
        table["change"] = change;
        table["kind"] = kind;
        table["item"] = item;
        table["amount"] = amount;
        table["old_amount"] = old_amount;
        table["object_id"] = object_id;
        table["known"] = change == -1 || before.has_value();
        table["x"] = static_cast<int>(std::floor((x + 8.0f) / 32.0f));
        table["y"] = static_cast<int>(std::floor((y + 8.0f) / 32.0f));
        table["by_me"] = session_.local_net_id() >= 0 && change == session_.local_net_id();
        if (change >= 0) {
            table["by_net_id"] = change;
            table["by_name"] = features::strip_colors(by_name);
        }
    });
}

void ModuleSystem::on_raw_packet(const event::Event& event, const bool from_server)
{
    const auto* raw{ dynamic_cast<const event::RawPacketEvent*>(&event) };
    if (!raw || !host_.watches_packets()) {
        return;
    }
    const auto view{ modules::packet_view::describe(raw->data) };
    if (!view) {
        return;
    }

    modules::ModuleHost::Packet packet{ from_server, *view };
    const auto outcome{ host_.packet(packet) };
    if (outcome.cancelled) {
        spdlog::debug("{} packet kept by a module", from_server ? "Server" : "Game");
        event.cancel();
        return;
    }
    if (!outcome.replaced) {
        return;
    }
    const auto changed{ modules::packet_view::rebuild(packet.view, raw->data) };
    event.cancel();
    std::ignore = send_raw(from_server, changed);
}

bool ModuleSystem::send_raw(const bool to_game, const std::span<const std::byte> data)
{
    if (data.size() < sizeof(std::uint32_t)) {
        return false;
    }
    return to_game ? session_.send_raw_to_client(data) : session_.send_raw_to_server(data);
}

void ModuleSystem::on_raw_client_packet(const event::Event& event)
{
    const auto* raw{ dynamic_cast<const event::RawPacketEvent*>(&event) };
    if (!raw || raw->data.size() < 4 + sizeof(packet::GameUpdatePacket)) {
        return;
    }
    std::uint32_t message_type{};
    std::memcpy(&message_type, raw->data.data(), sizeof(message_type));
    if (message_type != packet::NET_MESSAGE_GAME_PACKET) {
        return;
    }
    packet::GameUpdatePacket header{};
    std::memcpy(&header, raw->data.data() + 4, sizeof(header));
    if (header.type != packet::PACKET_CALL_FUNCTION) {
        return;
    }
    packet::PacketVariant variant{};
    if (!variant.deserialize(raw->data.subspan(4 + sizeof(header)))) {
        return;
    }
    const auto name{ variant.get<std::string>(0) };
    if (name != "OnConsoleMessage" && name != "OnTalkBubble" && name != "OnTextOverlay") {
        return;
    }
    const auto& args{ variant.get_variants() };
    std::string text;
    std::int32_t net_id{ -1 };
    for (std::size_t i{ 1 }; i < args.size(); ++i) {
        if (const auto* value{ std::get_if<std::string>(&args[i]) }) {
            text = *value;
        }
        else if (i == 1) {
            net_id = static_cast<std::int32_t>(variant.get_integer(1).value_or(-1));
        }
    }
    host_.emit("console", [&](sol::table& table) {
        table["kind"] = name;
        table["text"] = text;
        table["clean"] = features::strip_colors(text);
        table["net_id"] = net_id;
    });
}

void ModuleSystem::update()
{
    host_.update();

    if (std::exchange(settings_changed_, false)) {
        host_.emit("settings");
    }
    for (const auto& turn : wheel_.take()) {
        host_.emit("wheel", [&](sol::table& event) { event["notches"] = turn.notches; });
    }
}

void ModuleSystem::update_wheel()
{
    // A module that is no longer loaded lets go of the wheel.
    std::erase_if(wheel_modes_, [&](const auto& entry) { return !host_.loaded(entry.first); });
    std::string mode{ "off" };
    for (const auto& [module, wanted] : wheel_modes_) {
        if (wanted != "off") {
            mode = wanted;
        }
    }
    wheel_.set_mode(mode);
}

bool ModuleSystem::capture_wheel(const std::string& module_id, const std::string& mode)
{
    wheel_modes_[module_id] = mode;
    update_wheel();
    return WheelHook::available();
}

modules::WalkOptions ModuleSystem::walk_options() const
{
    const auto settings{ map_.settings() };
    return { settings.speed, settings.step_ms, settings.gravity, settings.jump_height, settings.air_jumps, std::string{ map::describe(settings.doors) } };
}

modules::WalkOptions ModuleSystem::set_walk_options(const modules::WalkOptions& options)
{
    handlers::MapHandler::Settings settings{ map_.settings() };
    settings.speed = options.speed;
    settings.step_ms = options.step_ms;
    settings.gravity = options.gravity;
    settings.jump_height = options.jump_height;
    settings.air_jumps = options.air_jumps;
    settings.doors = map::door_mode(options.doors).value_or(settings.doors);
    map_.set_settings(settings);
    return walk_options();
}

std::string ModuleSystem::save_setting(const std::string& module_id, const std::string& key, const modules::SettingValue& value)
{
    // The module's settings file, as you would edit it: other keys kept.
    const auto file{ config_.dir() / "modules" / (module_id + ".json") };
    const auto shown{ config_.shown(file) };
    modules::Settings values;
    std::error_code ec;
    if (std::filesystem::exists(file, ec)) {
        const auto text{ modules::json::read_text(file) };
        auto parsed{ text ? modules::json::parse<modules::Settings>(*text) : std::unexpected{ std::string{ "cannot be read" } } };
        if (!parsed) {
            spdlog::error("Config error: {}: {}", shown, parsed.error());
            return fmt::format("{} is broken ({}): fix it first", shown, parsed.error());
        }
        values = std::move(*parsed);
    }
    values[key] = value;

    std::string json;
    if (glz::write<glz::opts{ .prettify = true }>(values, json)) {
        return "could not write the settings";
    }
    std::filesystem::create_directories(file.parent_path(), ec);
    {
        std::ofstream out{ file, std::ios::binary | std::ios::trunc };
        out << json << '\n';
        if (!out) {
            return fmt::format("could not write {}", shown);
        }
    }
    spdlog::info("{}: {} = {} (changed in the game)", shown, key, modules::to_string(value));

    // Work the settings out again for the active profile; no module is
    // loaded or unloaded (the module asking is running right now).
    const auto resolved{ config_.resolve(profile_, host_.defaults()) };
    if (!resolved) {
        for (const auto& error : resolved.error()) {
            spdlog::error("Config error: {}: {}", error.file, error.reason);
        }
        return fmt::format("saved to {}, but another config file is broken (see the proxy log)", shown);
    }
    host_.refresh_settings(*resolved);
    settings_changed_ = true;

    const auto module{ resolved->settings.find(module_id) };
    if (module != resolved->settings.end()) {
        if (const auto it{ module->second.find(key) }; it != module->second.end() && it->second != value) {
            return fmt::format("saved to {}, but profile '{}' sets {} = {}", shown, profile_, key, modules::to_string(it->second));
        }
    }
    return {};
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
    settings_changed_ = true;
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
    view.server = server_name_;
    view.official = mods_.official();
    if (const auto player{ world::World::instance().get_local_player().lock() }) {
        view.user_id = static_cast<std::uint32_t>(player->user_id());
    }
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
    if (const auto* vending{ data->extra.get_as<world::tile_extra::Vending>() }) {
        info.extra = "vending";
        info.vend_item = vending->item_id;
        info.vend_price = static_cast<std::int32_t>(vending->price);
    }
    else if (const auto* lock{ data->extra.get_as<world::tile_extra::Lock>() }) {
        info.extra = "lock";
        info.lock_owner = lock->owner_id;
        info.lock_access = lock->accesses;
    }
    else if (const auto* door{ data->extra.get_as<world::tile_extra::Door>() }) {
        info.extra = "door";
        info.label = door->label;
    }
    else if (const auto* sign{ data->extra.get_as<world::tile_extra::Sign>() }) {
        info.extra = "sign";
        info.label = sign->label;
    }
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

modules::WalkView ModuleSystem::walk_to(const map::TilePos tile, const std::optional<bool> gravity)
{
    const auto result{ map_.walk_to(tile, gravity) };
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

namespace {
constexpr std::array<std::uint16_t, 4> AREA_LOCKS{ 202, 204, 206, 4994 };

template <typename Visit>
void each_tile(const Visit& visit)
{
    const auto& tile_map{ world::World::instance().get_tile_map() };
    const auto size{ tile_map.get_size() };
    const auto& tiles{ tile_map.get_tiles() };
    for (int y{ 0 }; y < size.y; ++y) {
        for (int x{ 0 }; x < size.x; ++x) {
            const auto index{ static_cast<std::size_t>(y) * static_cast<std::size_t>(size.x) + static_cast<std::size_t>(x) };
            if (index < tiles.size()) {
                visit(x, y, tiles[index]);
            }
        }
    }
}
}

std::vector<modules::VendingInfo> ModuleSystem::vendings() const
{
    std::vector<modules::VendingInfo> list;
    if (!session_.in_world()) {
        return list;
    }
    each_tile([&](const int x, const int y, const world::Tile& tile) {
        if (const auto* vending{ tile.extra.get_as<world::tile_extra::Vending>() }) {
            list.push_back({ x, y, tile.foreground, vending->item_id, static_cast<std::int32_t>(vending->price) });
        }
    });
    return list;
}

std::vector<modules::LockInfo> ModuleSystem::locks() const
{
    std::vector<modules::LockInfo> list;
    if (!session_.in_world()) {
        return list;
    }
    each_tile([&](const int x, const int y, const world::Tile& tile) {
        if (const auto* lock{ tile.extra.get_as<world::tile_extra::Lock>() }) {
            const bool area{ std::ranges::find(AREA_LOCKS, tile.foreground) != AREA_LOCKS.end() };
            list.push_back({ x, y, tile.foreground, lock->owner_id, lock->accesses, !area });
        }
    });
    return list;
}

modules::WorldSummary ModuleSystem::world_summary() const
{
    modules::WorldSummary summary{};
    if (!session_.in_world()) {
        return summary;
    }
    each_tile([&](int, int, const world::Tile& tile) {
        if (tile.foreground != 0) {
            ++summary.foreground[tile.foreground];
        }
        if (tile.background != 0) {
            ++summary.background[tile.background];
        }
    });
    return summary;
}

std::vector<modules::TileHit> ModuleSystem::find_tiles(const std::uint16_t item) const
{
    std::vector<modules::TileHit> hits;
    if (!session_.in_world() || item == 0) {
        return hits;
    }
    each_tile([&](const int x, const int y, const world::Tile& tile) {
        if (tile.foreground == item) {
            hits.push_back({ x, y, false });
        }
        if (tile.background == item) {
            hits.push_back({ x, y, true });
        }
    });
    return hits;
}

modules::RouteView ModuleSystem::route(const map::TilePos tile, const bool walk, const std::optional<bool> gravity)
{
    const auto route{ walk ? map_.walk_route(tile, gravity) : map_.reach_route(tile) };
    return { route.ok, route.tiles, route.reason };
}

std::vector<int> ModuleSystem::distances()
{
    return map_.reach_distances();
}

bool ModuleSystem::tile_action(const map::TilePos tile, const std::uint32_t item)
{
    const auto& world{ world::World::instance() };
    const auto me{ world.get_local_player().lock() };
    if (!session_.connected() || !session_.in_world() || !me) {
        return false;
    }
    // What the game sends when you use an item on a tile (the wrench: 32).
    packet::GameUpdatePacket header{};
    header.type = packet::PACKET_TILE_CHANGE_REQUEST;
    header.net_id = static_cast<std::uint32_t>(session_.local_net_id());
    header.pos_x = static_cast<float>(me->position().x);
    header.pos_y = static_cast<float>(me->position().y);
    header.int_x = tile.x;
    header.int_y = tile.y;
    header.int_data = static_cast<std::int32_t>(item);
    return session_.send_to_server(packet::GamePayload{ header, std::vector<std::byte>{} });
}

void ModuleSystem::set_overlay(const std::string& module_id, const std::string& id, const std::optional<modules::Overlay>& overlay)
{
    const auto key{ module_id + "/" + id };
    if (overlay) {
        // The same again (modules redraw often): nothing to publish.
        if (const auto it{ overlays_.find(key) }; it != overlays_.end() && it->second == *overlay) {
            return;
        }
        overlays_[key] = *overlay;
    }
    else if (!overlays_.erase(key)) {
        return;
    }
    publish_module_view();
}

void ModuleSystem::set_panel(const std::string& module_id, const std::string& id, const std::optional<std::string>& json)
{
    const auto key{ module_id + "/" + id };
    if (json) {
        if (const auto it{ panels_.find(key) }; it != panels_.end() && it->second == *json) {
            return;
        }
        panels_[key] = *json;
    }
    else if (!panels_.erase(key)) {
        return;
    }
    publish_module_view();
}

void ModuleSystem::request_pick(const std::string& module_id, const std::string& label)
{
    if (label.empty()) {
        if (pick_owner_ == module_id) {
            pick_owner_.clear();
            map_.set_pick({});
        }
        return;
    }
    pick_owner_ = module_id;
    map_.set_pick(label);
}

void ModuleSystem::forget_module_view(const bool everything)
{
    // What belongs to a world (marks, a pick) goes with it; tabs stay.
    overlays_.clear();
    pick_owner_.clear();
    map_.set_pick({});
    if (everything) {
        std::erase_if(panels_, [&](const auto& entry) { return !host_.loaded(entry.first.substr(0, entry.first.find('/'))); });
    }
    publish_module_view();
}

void ModuleSystem::publish_module_view()
{
    std::string json{ "{\"overlays\":[" };
    bool first{ true };
    for (const auto& [key, overlay] : overlays_) {
        json += first ? "" : ",";
        first = false;
        std::string markers;
        for (const auto& marker : overlay.markers) {
            markers += fmt::format("{}{{\"x\":{},\"y\":{},\"label\":{},\"color\":{}}}", markers.empty() ? "" : ",",
                marker.x, marker.y, glz::write_json(marker.label).value_or("\"\""), glz::write_json(marker.color).value_or("\"\""));
        }
        std::string route;
        for (const auto& tile : overlay.route) {
            route += fmt::format("{}[{},{}]", route.empty() ? "" : ",", tile.x, tile.y);
        }
        std::string areas;
        for (const auto& area : overlay.areas) {
            areas += fmt::format("{}{{\"x1\":{},\"y1\":{},\"x2\":{},\"y2\":{},\"label\":{},\"color\":{}}}", areas.empty() ? "" : ",",
                area.x1, area.y1, area.x2, area.y2, glz::write_json(area.label).value_or("\"\""), glz::write_json(area.color).value_or("\"\""));
        }
        json += fmt::format("{{\"key\":{},\"markers\":[{}],\"route\":[{}],\"route_color\":{},\"areas\":[{}]}}",
            glz::write_json(key).value_or("\"\""), markers, route, glz::write_json(overlay.route_color).value_or("\"\""), areas);
    }
    json += "],\"panels\":[";
    first = true;
    for (const auto& [key, spec] : panels_) {
        json += first ? "" : ",";
        first = false;
        json += fmt::format("{{\"key\":{},\"spec\":{}}}", glz::write_json(key).value_or("\"\""), spec);
    }
    json += "]}";
    map_.set_module_view(std::move(json));
}

std::string ModuleSystem::answer_panel(const std::map<std::string, std::string>& params)
{
    const auto key{ params.at("panel") };
    const auto slash{ key.find('/') };
    if (slash == std::string::npos || !panels_.contains(key)) {
        return R"({"ok":false,"message":"That tab is gone"})";
    }
    const auto module{ key.substr(0, slash) };
    const auto panel{ key.substr(slash + 1) };
    const auto get = [&](const char* name) {
        const auto it{ params.find(name) };
        return it == params.end() ? std::string{} : it->second;
    };
    const auto reply{ host_.emit_to(module, "panel", [&](sol::table& table) {
        table["panel"] = panel;
        table["action"] = get("action");
        table["row"] = get("row");
        table["value"] = get("value");
        sol::state_view lua{ table.lua_state() };
        sol::table values{ lua.create_table() };
        for (const auto& [name, value] : params) {
            if (name.starts_with("f_")) {
                values[name.substr(2)] = value;
            }
        }
        table["values"] = values;
    }) };
    if (!reply.ran) {
        return R"({"ok":false,"message":"The module does not answer its tab"})";
    }
    return fmt::format("{{\"ok\":{},\"message\":{}}}", reply.ok ? "true" : "false", glz::write_json(reply.message).value_or("\"\""));
}

std::string ModuleSystem::render_png(std::string& error)
{
    if (!session_.in_world()) {
        error = "you are not in a world";
        return {};
    }
    map::RenderInput input{};
    const auto now{ std::chrono::system_clock::now() };
    const auto seconds{ std::chrono::system_clock::to_time_t(now) };
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    const auto stamp{ fmt::format("{:04}-{:02}-{:02}_{:02}-{:02}-{:02}", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec) };
    input.title = fmt::format("{} - {}", session_.current_world(), stamp);
    for (const auto& [key, overlay] : overlays_) {
        for (const auto& marker : overlay.markers) {
            input.markers.push_back({ { marker.x, marker.y }, marker.label, marker.color });
        }
        if (overlay.route.size() > 1) {
            input.routes.push_back({ overlay.route, overlay.route_color });
        }
        for (const auto& area : overlay.areas) {
            input.areas.push_back({ { area.x1, area.y1 }, { area.x2, area.y2 }, area.label, area.color });
        }
    }

    const auto png{ map_.render_png(input) };
    if (png.empty()) {
        error = "nothing to draw";
        return {};
    }
    const auto dir{ data_dir_ / "renders" };
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::string world_name{ session_.current_world() };
    std::erase_if(world_name, [](const char c) { return !std::isalnum(static_cast<unsigned char>(c)); });
    const auto file{ dir / fmt::format("{}_{}.png", world_name.empty() ? "WORLD" : world_name, stamp) };
    std::ofstream out{ file, std::ios::binary | std::ios::trunc };
    out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    if (!out) {
        error = "could not write " + file.string();
        return {};
    }
    spdlog::info("Map exported to {}", file.string());
    return file.string();
}

modules::AbilitiesView ModuleSystem::abilities() const
{
    const auto status{ mods_.status() };
    const auto& s{ status.settings };
    return { status.allowed, status.why, status.game_server, status.state_known, status.state_made,
        { s.speed, s.gravity, s.acceleration, s.water_speed, s.double_jump, s.noclip, s.range, s.god } };
}

modules::AbilitiesView ModuleSystem::set_abilities(const modules::Abilities& a)
{
    std::ignore = mods_.set({ a.speed, a.gravity, a.acceleration, a.water_speed, a.double_jump, a.noclip, a.range, a.god });
    return abilities();
}

bool ModuleSystem::key_down(const std::string_view key) const
{
#ifdef _WIN32
    int code{ 0 };
    if (key == "alt") {
        code = VK_MENU;
    }
    else if (key == "ctrl") {
        code = VK_CONTROL;
    }
    else if (key == "shift") {
        code = VK_SHIFT;
    }
    return code != 0 && (GetAsyncKeyState(code) & 0x8000) != 0;
#else
    std::ignore = key;
    return false;
#endif
}

bool ModuleSystem::map_window() const
{
    return map_window_;
}
}
