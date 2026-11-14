#include "feature_handler.hpp"

#include <tuple>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "../features.hpp"
#include "../../packet/packet_helper.hpp"
#include "../../packet/game/dialog.hpp"
#include "../../packet/game/player.hpp"
#include "../../packet/game/world.hpp"
#include "../../packet/message/chat.hpp"
#include "../../packet/message/exit.hpp"
#include "../../world/world.hpp"

namespace core::handlers {
namespace {
constexpr auto WARP_EXIT_TIMEOUT{ std::chrono::seconds{ 3 } };

template <packet::PacketId Id>
const event::TypedPacketEvent<Id>* as(const event::Event& event, const event::Direction direction)
{
    const auto evt{ dynamic_cast<const event::TypedPacketEvent<Id>*>(&event) };
    if (!evt || evt->direction != direction) {
        return nullptr;
    }
    return evt;
}

std::string world_part(const std::string& target)
{
    return target.substr(0, target.find('|'));
}

// Used only when the server never told us a player's title JSON.
std::string default_title(const std::int32_t net_id)
{
    return fmt::format(
        R"({{"PlayerWorldID":{},"TitleTexture":"","TitleTextureCoordinates":"","WrenchCustomization":{{"WrenchForegroundCanRotate":false,"WrenchForegroundID":-1,"WrenchIconID":-1}}}})",
        net_id
    );
}
}

FeatureHandler::FeatureHandler(event::Dispatcher& dispatcher, network::Server& server, network::Client& client)
    : dispatcher_{ dispatcher }
    , server_{ server }
    , client_{ client }
{
    // Lowest priority: the world/player state handlers run first, and a
    // packet this handler rewrites is not processed twice.
    const auto listen = [this](const packet::PacketId id, void (FeatureHandler::*handler)(const event::Event&)) {
        const auto type{ event::packet_event_type(id) };
        handles_.emplace_back(
            dispatcher_,
            type,
            dispatcher_.append_listener(type, [this, handler](const event::Event& e) { (this->*handler)(e); }, event::Priority::Lowest)
        );
    };

    listen(packet::PacketId::OnSpawn, &FeatureHandler::on_spawn);
    listen(packet::PacketId::OnNameChanged, &FeatureHandler::on_name_changed);
    listen(packet::PacketId::OnRemove, &FeatureHandler::on_remove);
    listen(packet::PacketId::OnRequestWorldSelectMenu, &FeatureHandler::on_world_select_menu);
    listen(packet::PacketId::SendMapData, &FeatureHandler::on_map_data);
    listen(packet::PacketId::JoinRequest, &FeatureHandler::on_join_request);
    listen(packet::PacketId::State, &FeatureHandler::on_state);

    for (const auto type : { event::Type::ClientDisconnect, event::Type::ServerDisconnect }) {
        handles_.emplace_back(
            dispatcher_,
            type,
            dispatcher_.append_listener(type, [this](const event::Event&) { on_disconnect(); })
        );
    }
}

bool FeatureHandler::warp(const std::string& target)
{
    if (!client_.is_connected()) {
        return false;
    }

    if (!in_world_) {
        pending_warp_.clear();
        send_join(target);
        return true;
    }

    // Leave first, like pressing Exit; the join follows once the server shows
    // the world menu (or after a short timeout if it never says so).
    pending_warp_ = target;
    warp_deadline_ = std::chrono::steady_clock::now() + WARP_EXIT_TIMEOUT;
    packet::message::QuitToExit quit{};
    send_to_server(quit.write());
    return true;
}

void FeatureHandler::set_nick(const std::string& nick)
{
    nick_ = nick;
    refresh_name(local_net_id_);
}

void FeatureHandler::reset_nick()
{
    nick_.clear();
    refresh_name(local_net_id_);
}

void FeatureHandler::set_show_net_ids(const bool show)
{
    show_net_ids_ = show;
    for (const auto& [net_id, player] : players_) {
        refresh_name(net_id);
    }
}

void FeatureHandler::message(const std::string& text) const
{
    packet::message::Log log{};
    log.msg = text;
    send_to_client(log.write());
}

void FeatureHandler::show_dialog(const std::string& markup) const
{
    packet::game::OnDialogRequest dialog{};
    dialog.dialog = markup;
    send_to_client(dialog.write());
}

void FeatureHandler::update()
{
    if (pending_warp_.empty() || std::chrono::steady_clock::now() < warp_deadline_) {
        return;
    }

    const std::string target{ std::move(pending_warp_) };
    pending_warp_.clear();
    if (client_.is_connected()) {
        spdlog::debug("Warp: no world menu from the server, joining {} anyway", target);
        send_join(target);
    }
}

void FeatureHandler::send_to_client(const packet::Payload& payload) const
{
    auto data{ packet::PacketHelper::serialize(payload) };
    if (data.empty()) {
        return;
    }
    data.push_back(std::byte{ 0 });
    std::ignore = server_.write(data);
}

void FeatureHandler::send_to_server(const packet::Payload& payload) const
{
    auto data{ packet::PacketHelper::serialize(payload) };
    if (data.empty()) {
        return;
    }
    data.push_back(std::byte{ 0 });
    std::ignore = client_.write(data);
}

void FeatureHandler::send_join(const std::string& target)
{
    spdlog::info("Warping to {}", target);
    packet::message::JoinRequest join{};
    join.world_name = target;
    join.invited_world = false;
    send_to_server(join.write());
}

std::string FeatureHandler::shown_name(const std::int32_t net_id, const std::string& server_name) const
{
    std::string name{ net_id == local_net_id_ && !nick_.empty() ? features::colored_name(nick_) : server_name };
    if (show_net_ids_) {
        name += fmt::format(" `4[{}]``", net_id);
    }
    return name;
}

void FeatureHandler::refresh_name(const std::int32_t net_id)
{
    const auto it{ players_.find(net_id) };
    if (net_id < 0 || it == players_.end()) {
        return;
    }

    packet::game::OnNameChanged pkt{};
    pkt.net_id = net_id;
    pkt.name = shown_name(net_id, it->second.name);
    pkt.title = it->second.title.empty() ? default_title(net_id) : it->second.title;
    send_to_client(pkt.write());
}

void FeatureHandler::on_spawn(const event::Event& event)
{
    const auto evt{ as<packet::PacketId::OnSpawn>(event, event::Direction::ClientBound) };
    const auto pkt{ evt ? evt->get<packet::game::OnSpawn>() : nullptr };
    if (!pkt || pkt->variant.size() < 2) {
        return;
    }

    players_[pkt->net_id] = KnownPlayer{ pkt->name, pkt->title_icon };
    if (pkt->type == "local") {
        local_net_id_ = pkt->net_id;
        real_name_ = pkt->name;
        local_position_ = glm::vec2{ pkt->position.x, pkt->position.y };
    }

    const std::string name{ shown_name(pkt->net_id, pkt->name) };
    if (name == pkt->name) {
        return;
    }

    // Change only the name line; every other field goes to the game as the
    // server sent it.
    const auto text{ features::replace_field(pkt->variant.get<std::string>(1), "name", name) };
    if (!text) {
        return;
    }

    packet::PacketVariant variant{ pkt->variant };
    variant.set(1, *text);
    evt->cancel();
    send_to_client(packet::VariantPayload{ pkt->game_packet, variant });
}

void FeatureHandler::on_name_changed(const event::Event& event)
{
    const auto evt{ as<packet::PacketId::OnNameChanged>(event, event::Direction::ClientBound) };
    const auto pkt{ evt ? evt->get<packet::game::OnNameChanged>() : nullptr };
    if (!pkt) {
        return;
    }

    auto& player{ players_[pkt->net_id] };
    player.name = pkt->name;
    if (!pkt->title.empty()) {
        player.title = pkt->title;
    }
    if (pkt->net_id == local_net_id_) {
        real_name_ = pkt->name;
    }

    const std::string name{ shown_name(pkt->net_id, pkt->name) };
    if (name == pkt->name) {
        return;
    }

    pkt->name = name;
    evt->cancel();
    send_to_client(pkt->write());
}

void FeatureHandler::on_remove(const event::Event& event)
{
    const auto evt{ as<packet::PacketId::OnRemove>(event, event::Direction::ClientBound) };
    if (const auto pkt{ evt ? evt->get<packet::game::OnRemove>() : nullptr }) {
        players_.erase(pkt->net_id);
    }
}

void FeatureHandler::on_world_select_menu(const event::Event& event)
{
    if (!as<packet::PacketId::OnRequestWorldSelectMenu>(event, event::Direction::ClientBound)) {
        return;
    }

    in_world_ = false;
    players_.clear();
    local_net_id_ = -1;

    if (!pending_warp_.empty()) {
        const std::string target{ std::move(pending_warp_) };
        pending_warp_.clear();
        send_join(target);
    }
}

void FeatureHandler::on_map_data(const event::Event& event)
{
    if (!as<packet::PacketId::SendMapData>(event, event::Direction::ClientBound)) {
        return;
    }

    std::string name{ world::World::instance().get_name() };
    if (!features::normalize_world_name(name)) {
        name = world_part(last_join_);
    }

    in_world_ = true;
    players_.clear();
    local_net_id_ = -1;
    local_position_.reset();

    if (!name.empty() && name != current_world_) {
        if (!current_world_.empty()) {
            previous_world_ = current_world_;
        }
        current_world_ = name;
    }
}

void FeatureHandler::on_join_request(const event::Event& event)
{
    const auto evt{ as<packet::PacketId::JoinRequest>(event, event::Direction::ServerBound) };
    if (const auto pkt{ evt ? evt->get<packet::message::JoinRequest>() : nullptr }) {
        if (const auto name{ features::normalize_world_name(pkt->world_name) }) {
            last_join_ = *name;
        }
    }
}

void FeatureHandler::on_state(const event::Event& event)
{
    const auto evt{ as<packet::PacketId::State>(event, event::Direction::ServerBound) };
    if (const auto pkt{ evt ? evt->get<packet::game::State>() : nullptr }; pkt && in_world_) {
        local_position_ = glm::vec2{ pkt->game_packet.pos_x, pkt->game_packet.pos_y };
    }
}

void FeatureHandler::on_disconnect()
{
    in_world_ = false;
    pending_warp_.clear();
    players_.clear();
    local_net_id_ = -1;
}
}
