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
    SessionHandler& session,
    const Config::MapConfig& config,
    map::Bridge& bridge,
    std::optional<std::string> window_url
)
    : dispatcher_{ dispatcher }
    , server_{ server }
    , client_{ client }
    , session_{ session }
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

    // Before the world and session handlers: a held-back state packet must
    // not move you on the proxy's map either.
    listen(packet::PacketId::State, event::Priority::FairlyHigh, [this](const event::Event& e) { on_state(e); });

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
    const glm::ivec2 size{ session_.in_world() ? world::World::instance().get_tile_map().get_size() : glm::ivec2{ 0, 0 } };
    const auto target{ map::resolve_click(click, size) };

    // A right click goes to the modules for any tile - also outside the
    // world or with no world yet (modules/reachability answers both).
    if (click.button == map::MouseButton::Right && click.view.valid()) {
        if (target) {
            return module_click(click.button, target->tile);
        }
        if (target.error() == map::ClickError::NoWorld || target.error() == map::ClickError::OutsideWorld) {
            return module_click(click.button, map::cursor_tile(click.view, click.screen));
        }
    }

    if (!target) {
        return Result{ false, "click", std::nullopt, fmt::format("Nothing to do: {}.", map::describe(target.error())) };
    }
    return module_click(click.button, target->tile);
}

MapHandler::Result MapHandler::module_click(const map::MouseButton button, const map::TilePos tile)
{
    const char* which{ button == map::MouseButton::Right ? "Right" : "Left" };
    const char* action{ button == map::MouseButton::Right ? "right_click" : "left_click" };
    const auto answer{ click_hook_ ? click_hook_(button, tile) : ClickAnswer{} };
    if (!answer.handled) {
        Result result{ false, action, tile, fmt::format("{} click does nothing: no module handles it (modules/, config/modules.json).", which) };
        result.kind = "none";
        result.marker = false;
        return result;
    }
    if (!answer.card) {
        Result result{ false, action, tile, fmt::format("No module answered this {} click (see the proxy log).", button == map::MouseButton::Right ? "right" : "left") };
        result.marker = false;
        return result;
    }

    const auto& card{ *answer.card };
    Result result{ card.kind == "ok", card.toast ? "toast" : "card", card.tile.value_or(tile), card.title };
    result.detail = card.text;
    result.reach = card.status;
    result.kind = card.kind;
    result.marker = card.marker;
    return result;
}

MapHandler::ReachReport MapHandler::check_reach(const map::TilePos target)
{
    ReachReport report{};
    report.target = target;

    // What the check needs: the world's tiles, which blocks collide
    // (items.dat) and where you are.
    const auto& world{ world::World::instance() };
    if (!session_.in_world() || grid().empty()) {
        report.missing = "карта мира ещё не получена";
    }
    else if (item::ItemDatabase::instance().empty()) {
        report.missing = "items.dat не загружен, неизвестно, какие блоки твёрдые";
    }
    else if (const auto me{ world.get_local_player().lock() }) {
        report.position = glm::vec2{ me->position() };
        report.body = map::Body::from_colrect(me->collision());
    }
    else {
        report.missing = "позиция игрока ещё не получена";
    }

    if (report.missing.empty()) {
        report.result = map::check_reach(grid(), report.position, report.body, target);
        if (report.result.status == map::Reach::NoData) {
            report.missing = "позиция игрока вне карты";
        }
    }

    if (report.result.status != map::Reach::NoData) {
        if (const auto tile{ inspect(target) }; tile.ok) {
            report.tile_text = tile.message.substr(tile.message.find(": ") + 2);
        }
    }
    return report;
}

std::string MapHandler::ReachReport::message() const
{
    std::string text{ map::headline(result.status) };
    if (const auto why{ map::reason(result.status) }; !why.empty()) {
        text += fmt::format(": {}", why);
    }
    return text;
}

std::string MapHandler::ReachReport::summary() const
{
    return fmt::format(
        "start {} at {}, target {}, {} -> {}{}",
        result.start ? fmt::format("tile {}, {}", result.start->x, result.start->y) : std::string{ "tile unknown" },
        position ? fmt::format("{}, {} px (body {}x{})", position->x, position->y, body.size.x, body.size.y) : std::string{ "unknown position" },
        target.x,
        target.y,
        message(),
        result.status == map::Reach::Reachable
            ? fmt::format(" ({} steps)", result.steps)
            : result.status == map::Reach::NoData ? fmt::format(" ({})", missing) : std::string{}
    );
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
    if (!session_.in_world()) {
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
    const auto* data{ session_.in_world() ? world.get_tile(tile.x, tile.y) : nullptr };
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
    if (walk_end_hook_) {
        walk_end_hook_(message, tell_game);
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
        request.reply.set_value(map::result_json(result.ok, result.action, result.tile, result.message, result.reach, result.detail, result.kind, result.marker));
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
    view.in_world = session_.in_world();
    view.world_name = session_.current_world();
    view.tiles_revision = tiles_revision_;
    view.gravity = path_options_.gravity;
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
