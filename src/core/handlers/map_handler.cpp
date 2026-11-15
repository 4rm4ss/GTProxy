#include "map_handler.hpp"

#include <algorithm>
#include <tuple>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "../features.hpp"
#include "../../item/item_database.hpp"
#include "../../map/snapshot.hpp"
#include "../../packet/packet_helper.hpp"
#include "../../packet/game/player.hpp"
#include "../../packet/game/world.hpp"
#include "../../world/world.hpp"

namespace core::handlers {
namespace {
constexpr float MIN_SPEED{ 16.0f };
constexpr float MAX_SPEED{ 1000.0f };
constexpr int MIN_STEP_MS{ 30 };
constexpr int MAX_STEP_MS{ 1000 };
// A window counts as open while it polls at least this often.
constexpr auto WINDOW_TIMEOUT{ std::chrono::seconds{ 5 } };
constexpr auto PUBLISH_INTERVAL{ std::chrono::milliseconds{ 150 } };

template <packet::PacketId Id>
const event::TypedPacketEvent<Id>* as(const event::Event& event, const event::Direction direction)
{
    const auto evt{ dynamic_cast<const event::TypedPacketEvent<Id>*>(&event) };
    if (!evt || evt->direction != direction) {
        return nullptr;
    }
    return evt;
}

std::string item_label(const std::uint16_t item_id)
{
    if (const auto* info{ item::ItemDatabase::instance().get_item(item_id) }) {
        return fmt::format("{} ({})", info->item_name, item_id);
    }
    return fmt::format("item {}", item_id);
}

std::string tiles_word(const std::size_t count)
{
    return fmt::format("{} tile{}", count, count == 1 ? "" : "s");
}
}

MapHandler::MapHandler(
    event::Dispatcher& dispatcher,
    network::Server& server,
    network::Client& client,
    FeatureHandler& features,
    const Config::MapConfig& config,
    map::Bridge& bridge,
    std::optional<std::string> window_url
)
    : dispatcher_{ dispatcher }
    , server_{ server }
    , client_{ client }
    , features_{ features }
    , bridge_{ bridge }
    , window_url_{ std::move(window_url) }
    , walk_speed_{ std::clamp(config.walk_speed, MIN_SPEED, MAX_SPEED) }
    , step_interval_{ std::clamp(config.step_interval_ms, MIN_STEP_MS, MAX_STEP_MS) }
{
    path_options_.gravity = config.gravity;
    path_options_.jump_height = std::clamp(config.jump_height, 1, 20);

    const auto listen = [this](const packet::PacketId id, const std::int8_t priority, auto handler) {
        const auto type{ event::packet_event_type(id) };
        handles_.emplace_back(dispatcher_, type, dispatcher_.append_listener(type, handler, priority));
    };

    // Before the world and feature handlers: a held-back state packet must
    // not move you on the proxy's map either.
    listen(packet::PacketId::State, event::Priority::FairlyHigh, [this](const event::Event& e) { on_state(e); });
    listen(packet::PacketId::TileChangeRequest, event::Priority::FairlyHigh, [this](const event::Event& e) { on_tile_change_request(e); });

    // After the world handler has applied the change.
    listen(packet::PacketId::OnSetPos, event::Priority::Lowest, [this](const event::Event& e) { on_set_pos(e); });
    listen(packet::PacketId::SendTileUpdateData, event::Priority::Lowest, [this](const event::Event&) { mark_tiles_changed(); });
    listen(packet::PacketId::TileChangeRequest, event::Priority::Lowest, [this](const event::Event& e) {
        if (as<packet::PacketId::TileChangeRequest>(e, event::Direction::ClientBound)) {
            mark_tiles_changed();
        }
    });
    for (const auto id : { packet::PacketId::SendMapData, packet::PacketId::OnRequestWorldSelectMenu, packet::PacketId::JoinRequest }) {
        listen(id, event::Priority::Lowest, [this](const event::Event&) { on_world_change(); });
    }
    for (const auto type : { event::Type::ClientDisconnect, event::Type::ServerDisconnect }) {
        handles_.emplace_back(dispatcher_, type, dispatcher_.append_listener(type, [this](const event::Event&) {
            model_.reset();
            on_world_change();
        }));
    }

    spdlog::info(
        "Map: walking at {} px/s, a step every {} ms, {}",
        walk_speed_,
        step_interval_.count(),
        path_options_.gravity
            ? fmt::format("on foot (jumps up to {} tiles)", path_options_.jump_height)
            : std::string{ "through any free tile" }
    );
}

MapHandler::Result MapHandler::handle_click(const map::Click& click)
{
    const glm::ivec2 size{ features_.in_world() ? world::World::instance().get_tile_map().get_size() : glm::ivec2{ 0, 0 } };
    const auto target{ map::resolve_click(click, size) };

    // Right click: the reachability check answers for any tile - also
    // outside the world (rejected) or with no world yet (no data).
    if (click.button == map::MouseButton::Right && click.view.valid()) {
        if (target) {
            return check_reach(target->tile, "map window right click");
        }
        if (target.error() == map::ClickError::NoWorld || target.error() == map::ClickError::OutsideWorld) {
            return check_reach(map::cursor_tile(click.view, click.screen), "map window right click");
        }
    }

    if (!target) {
        return Result{ false, "click", std::nullopt, fmt::format("Nothing to do: {}.", map::describe(target.error())) };
    }

    if (target->action == map::ClickAction::Inspect) {
        return check_reach(target->tile, "map window right click");
    }
    return walk_to(target->tile);
}

MapHandler::Result MapHandler::check_reach(const map::TilePos target, const std::string_view source)
{
    Result result{ false, "reach", target, {} };

    // What the check needs: the world's tiles, which blocks collide
    // (items.dat) and where you are.
    const auto& world{ world::World::instance() };
    std::string missing;
    std::optional<glm::vec2> position;
    map::Body body{};
    if (!features_.in_world() || grid().empty()) {
        missing = "карта мира ещё не получена";
    }
    else if (item::ItemDatabase::instance().empty()) {
        missing = "items.dat не загружен, неизвестно, какие блоки твёрдые";
    }
    else if (const auto me{ world.get_local_player().lock() }) {
        position = glm::vec2{ me->position() };
        body = map::Body::from_colrect(me->collision());
    }
    else {
        missing = "позиция игрока ещё не получена";
    }

    const auto reach{ missing.empty() ? map::check_reach(grid(), position, body, target) : map::ReachResult{} };
    if (missing.empty() && reach.status == map::Reach::NoData) {
        missing = "позиция игрока вне карты";
    }

    result.ok = reach.status == map::Reach::Reachable;
    result.reach = map::code(reach.status);
    result.message = std::string{ map::headline(reach.status) };
    if (const auto why{ map::reason(reach.status) }; !why.empty()) {
        result.message += fmt::format(": {}", why);
    }

    if (reach.status == map::Reach::NoData) {
        result.detail = fmt::format("Нет данных: {}.", missing);
    }
    else {
        result.detail = fmt::format("От {}, {} до {}, {}", reach.start->x, reach.start->y, target.x, target.y);
        if (reach.status == map::Reach::Reachable) {
            result.detail += fmt::format(": {} {}", reach.steps, map::steps_word(reach.steps));
        }
        result.detail += '.';
        if (const auto tile{ inspect(target) }; tile.ok) {
            result.detail += ' ';
            result.detail += tile.message.substr(tile.message.find(": ") + 2);
        }
    }

    spdlog::info(
        "Reach check ({}): start {} at {}, target {}, {} -> {}{}",
        source,
        reach.start ? fmt::format("tile {}, {}", reach.start->x, reach.start->y) : std::string{ "tile unknown" },
        position ? fmt::format("{}, {} px (body {}x{})", position->x, position->y, body.size.x, body.size.y) : std::string{ "unknown position" },
        target.x,
        target.y,
        result.message,
        reach.status == map::Reach::Reachable
            ? fmt::format(" ({} steps)", reach.steps)
            : reach.status == map::Reach::NoData ? fmt::format(" ({})", missing) : std::string{}
    );
    return result;
}

MapHandler::Result MapHandler::walk_to(const map::TilePos tile)
{
    auto result{ plan_walk(tile) };
    status_ = result.message;
    return result;
}

MapHandler::Result MapHandler::plan_walk(const map::TilePos tile)
{
    Result result{ false, "walk", tile, {} };

    if (!client_.is_connected()) {
        result.message = "Not connected to the game server.";
        return result;
    }
    if (!features_.in_world()) {
        result.message = "You are not in a world.";
        return result;
    }

    const auto& world{ world::World::instance() };
    const auto me{ world.get_local_player().lock() };
    if (!me || world.get_local_net_id() < 0) {
        result.message = "Your avatar is not known yet - wait until the world has loaded.";
        return result;
    }

    const auto& walk_grid{ grid() };
    const glm::vec2 position{ me->position() };
    const map::TilePos start{ map::player_tile(position) };
    const auto destination{ map::find_destination(walk_grid, start, tile, path_options_) };
    if (!destination) {
        result.message = fmt::format("Can't walk to {}, {}: {}.", tile.x, tile.y, map::describe(destination.error()));
        return result;
    }

    result.ok = true;
    result.tile = destination->tile;
    if (destination->path.size() < 2) {
        // A walk in progress ends here.
        if (walk_) {
            walk_.reset();
        }
        result.message = fmt::format("You are already at {}, {}.", destination->tile.x, destination->tile.y);
        return result;
    }

    // Start from where the avatar really is, not from its tile's corner.
    auto points{ map::path_to_points(destination->path) };
    points.front() = position;

    const auto now{ std::chrono::steady_clock::now() };
    const bool was_walking{ walk_.has_value() };
    walk_ = Walk{
        map::Walker{ std::move(points), walk_speed_ },
        destination->path,
        destination->tile,
        destination->next_to_goal,
        was_walking && walk_->facing_left,
        now,
        now + step_interval_,
    };
    last_target_ = destination->tile;

    const auto steps{ destination->path.size() - 1 };
    result.message = destination->next_to_goal
        ? fmt::format("Walking next to {}, {}: to {}, {} ({}).", tile.x, tile.y, destination->tile.x, destination->tile.y, tiles_word(steps))
        : fmt::format("Walking to {}, {} ({}).", destination->tile.x, destination->tile.y, tiles_word(steps));
    spdlog::info("Map: {} From {}, {}.", result.message, start.x, start.y);
    return result;
}

MapHandler::Result MapHandler::inspect(const map::TilePos tile) const
{
    Result result{ false, "inspect", tile, {} };

    const auto& world{ world::World::instance() };
    const auto* data{ features_.in_world() ? world.get_tile(tile.x, tile.y) : nullptr };
    if (!data) {
        result.message = fmt::format("No tile {}, {} in this world.", tile.x, tile.y);
        return result;
    }

    result.ok = true;
    std::string text{ fmt::format("Tile {}, {}: ", tile.x, tile.y) };
    if (data->foreground != 0) {
        text += item_label(data->foreground);
        text += data->background != 0 ? fmt::format(" on {}", item_label(data->background)) : std::string{};
    }
    else {
        text += data->background != 0 ? item_label(data->background) : std::string{ "empty" };
    }
    text += fmt::format(" - {}", map::describe(map::classify(*data)));
    if (world::has_flag(data->flag, world::TileFlag::Locked)) {
        text += ", locked";
    }
    if (world::has_flag(data->flag, world::TileFlag::Public)) {
        text += ", public";
    }
    text += '.';

    for (const auto& [net_id, player] : world.get_players()) {
        if (player && (player->invisible() == 0 || player->is_local()) && map::player_tile(player->position()) == tile) {
            text += fmt::format(" {} is here.", features::strip_colors(player->name()));
        }
    }

    for (const auto& object : world.get_object_map().get_objects()) {
        if (map::world_to_tile(object.pos + glm::vec2{ 8.0f }) == tile) {
            text += fmt::format(" Dropped: {} x{}.", item_label(object.item_id), object.amount);
        }
    }

    result.message = std::move(text);
    return result;
}

MapHandler::Result MapHandler::stop()
{
    if (!walk_) {
        status_ = "Not walking.";
        return Result{ true, "stop", std::nullopt, status_ };
    }

    const auto at{ map::player_tile(walk_->walker.position()) };
    end_walk(fmt::format("Stopped at {}, {}.", at.x, at.y), false);
    return Result{ true, "stop", at, status_ };
}

void MapHandler::set_click_walk(const bool on)
{
    click_walk_ = on;
}

void MapHandler::update()
{
    serve_window();

    const auto now{ std::chrono::steady_clock::now() };
    if (walk_) {
        step_walk(now);
    }

    publish(now);
}

const map::Grid& MapHandler::grid()
{
    if (grid_revision_ != tiles_revision_) {
        grid_ = map::Grid::from_tile_map(world::World::instance().get_tile_map());
        grid_revision_ = tiles_revision_;
    }
    return grid_;
}

void MapHandler::mark_tiles_changed()
{
    ++tiles_revision_;
    if (walk_) {
        check_path();
    }
}

void MapHandler::check_path()
{
    // A block placed on the way: find a new way from here.
    const auto& walk_grid{ grid() };
    const auto& path{ walk_->path };
    const auto from{ std::max<std::size_t>(walk_->walker.next_point(), 1) };
    for (std::size_t i{ from }; i < path.size(); ++i) {
        const auto cell{ walk_grid.at(path[i]) };
        const bool last{ i + 1 == path.size() };
        if (map::is_passable(cell) || (last && map::can_end_in(cell))) {
            continue;
        }

        const auto target{ walk_->target };
        if (const auto result{ walk_to(target) }; !result.ok) {
            end_walk(fmt::format("The way to {}, {} got blocked.", target.x, target.y), true);
        }
        return;
    }
}

void MapHandler::end_walk(const std::string& message, const bool tell_game)
{
    walk_.reset();
    status_ = message;
    spdlog::info("Map: {}", message);
    if (tell_game) {
        features_.message(fmt::format("`o[map] {}", message));
    }
}

void MapHandler::step_walk(const std::chrono::steady_clock::time_point now)
{
    if (now < walk_->next_step) {
        return;
    }

    auto& world{ world::World::instance() };
    const auto me{ world.get_local_player().lock() };
    if (!client_.is_connected() || !me) {
        end_walk("Walk stopped: you left the world.", false);
        return;
    }

    const float seconds{ std::min(std::chrono::duration<float>(now - walk_->last_step).count(), 1.0f) };
    walk_->last_step = now;
    walk_->next_step = now + step_interval_;

    const auto step{ walk_->walker.advance(seconds) };
    if (step.velocity.x < 0.0f) {
        walk_->facing_left = true;
    }
    else if (step.velocity.x > 0.0f) {
        walk_->facing_left = false;
    }

    const map::TilePos tile{ map::player_tile(step.position) };
    map::MoveState state{};
    state.position = step.position;
    state.velocity = step.velocity;
    state.facing_left = walk_->facing_left;
    state.on_ground = map::can_stand_on(grid().at(map::TilePos{ tile.x, tile.y + 1 }));

    const std::int32_t net_id{ world.get_local_net_id() };
    packet::GamePayload to_server{};
    to_server.packet = map::make_state_packet(model_.value_or(packet::GameUpdatePacket{}), net_id, state);
    send_to_server(to_server);

    packet::game::OnSetPos set_pos{};
    set_pos.net_id = net_id;
    set_pos.position = step.position;
    send_to_client(set_pos.write());

    me->set_position(step.position);

    if (walk_->walker.finished()) {
        const auto target{ walk_->target };
        const bool beside{ walk_->next_to_goal };
        end_walk(fmt::format("Arrived at {}, {}{}.", target.x, target.y, beside ? " (next to the block)" : ""), true);
    }
}

void MapHandler::serve_window()
{
    for (auto& request : bridge_.take_requests()) {
        const Result result{
            request.kind == map::Bridge::Request::Kind::Stop ? stop() : handle_click(request.click)
        };
        if (result.ok && result.action == "walk") {
            features_.message(fmt::format("`o[map] {}", result.message));
        }
        request.reply.set_value(map::result_json(result.ok, result.action, result.tile, result.message, result.reach, result.detail));
    }
}

void MapHandler::publish(const std::chrono::steady_clock::time_point now)
{
    if (now < next_publish_ || !bridge_.viewed_within(WINDOW_TIMEOUT)) {
        return;
    }
    next_publish_ = now + PUBLISH_INTERVAL;

    const auto& world{ world::World::instance() };
    if (published_tiles_ != tiles_revision_) {
        bridge_.publish_tiles(map::tiles_json(tiles_revision_, world, grid()));
        published_tiles_ = tiles_revision_;
    }

    map::StateView view{};
    view.connected = client_.is_connected();
    view.in_world = features_.in_world();
    view.world_name = features_.current_world();
    view.tiles_revision = tiles_revision_;
    view.gravity = path_options_.gravity;
    view.click_walk = click_walk_;
    view.status = status_;
    view.walk.target = last_target_;
    if (walk_) {
        view.walk.walking = true;
        const auto& path{ walk_->path };
        const auto from{ std::max<std::size_t>(walk_->walker.next_point(), 1) - 1 };
        view.walk.path.assign(path.begin() + static_cast<std::ptrdiff_t>(std::min(from, path.size())), path.end());
    }

    bridge_.publish_state(map::state_json(view, world));
}

void MapHandler::send_to_server(const packet::Payload& payload) const
{
    auto data{ packet::PacketHelper::serialize(payload) };
    if (data.empty()) {
        return;
    }
    data.push_back(std::byte{ 0 });
    std::ignore = client_.write(data);
}

void MapHandler::send_to_client(const packet::Payload& payload) const
{
    auto data{ packet::PacketHelper::serialize(payload) };
    if (data.empty()) {
        return;
    }
    data.push_back(std::byte{ 0 });
    std::ignore = server_.write(data);
}

void MapHandler::on_state(const event::Event& event)
{
    const auto evt{ as<packet::PacketId::State>(event, event::Direction::ServerBound) };
    const auto pkt{ evt ? evt->get<packet::game::State>() : nullptr };
    if (!pkt) {
        return;
    }

    if (walk_) {
        // The proxy moves you right now; the game's idea of where you are
        // would pull you back.
        evt->cancel();
        return;
    }

    model_ = pkt->game_packet;
}

void MapHandler::on_tile_change_request(const event::Event& event)
{
    const auto evt{ as<packet::PacketId::TileChangeRequest>(event, event::Direction::ServerBound) };
    const auto pkt{ evt ? evt->get<packet::game::TileChangeRequest>() : nullptr };
    if (!pkt || !click_walk_) {
        return;
    }

    // Click-walk: a click on a tile in the game walks there instead of
    // punching or building.
    evt->cancel();
    const auto result{ walk_to(map::TilePos{ pkt->int_x, pkt->int_y }) };
    features_.message(fmt::format("`o[map] {}", result.message));
}

void MapHandler::on_set_pos(const event::Event& event)
{
    const auto evt{ as<packet::PacketId::OnSetPos>(event, event::Direction::ClientBound) };
    const auto pkt{ evt ? evt->get<packet::game::OnSetPos>() : nullptr };
    if (!pkt || !walk_) {
        return;
    }

    const auto& world{ world::World::instance() };
    if (pkt->net_id != world.get_local_net_id() && !world.get_player(pkt->net_id).expired()) {
        return; // someone else
    }

    const auto at{ map::player_tile(pkt->position) };
    end_walk(fmt::format("Walk stopped: the server put you at {}, {}.", at.x, at.y), true);
}

void MapHandler::on_world_change()
{
    if (walk_) {
        end_walk("Walk stopped: you left the world.", false);
    }
    last_target_.reset();
    mark_tiles_changed();
}
}
