#include "session_handler.hpp"

#include <algorithm>
#include <cstring>
#include <tuple>

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
template <packet::PacketId Id>
const event::TypedPacketEvent<Id>* as(const event::Event& event, const event::Direction direction)
{
    const auto evt{ dynamic_cast<const event::TypedPacketEvent<Id>*>(&event) };
    if (!evt || evt->direction != direction) {
        return nullptr;
    }
    return evt;
}
}

SessionHandler::SessionHandler(event::Dispatcher& dispatcher, network::Server& server, network::Client& client)
    : dispatcher_{ dispatcher }
    , server_{ server }
    , client_{ client }
{
    // After the world handler: the world's name is parsed by then.
    const auto listen = [this](const packet::PacketId id, void (SessionHandler::*handler)(const event::Event&)) {
        const auto type{ event::packet_event_type(id) };
        handles_.emplace_back(
            dispatcher_,
            type,
            dispatcher_.append_listener(type, [this, handler](const event::Event& e) { (this->*handler)(e); }, event::Priority::FairlyLow)
        );
    };

    listen(packet::PacketId::OnSpawn, &SessionHandler::on_spawn);
    listen(packet::PacketId::OnRequestWorldSelectMenu, &SessionHandler::on_world_select_menu);
    listen(packet::PacketId::SendMapData, &SessionHandler::on_map_data);
    listen(packet::PacketId::JoinRequest, &SessionHandler::on_join_request);
    listen(packet::PacketId::State, &SessionHandler::on_state);

    for (const auto type : { event::Type::ClientDisconnect, event::Type::ServerDisconnect }) {
        handles_.emplace_back(
            dispatcher_,
            type,
            dispatcher_.append_listener(type, [this](const event::Event&) { on_disconnect(); })
        );
    }
}

void SessionHandler::message(const std::string& text) const
{
    packet::message::Log log{};
    log.msg = text;
    std::ignore = send_to_client(log.write());
}

void SessionHandler::show_dialog(const std::string& markup) const
{
    packet::game::OnDialogRequest dialog{};
    dialog.dialog = markup;
    std::ignore = send_to_client(dialog.write());
}

bool SessionHandler::send_raw_to_client(const std::span<const std::byte> data) const
{
    return server_.write(data);
}

bool SessionHandler::send_raw_to_server(const std::span<const std::byte> data) const
{
    return client_.write(data);
}

bool SessionHandler::send_text_to_server(const std::string& text, const packet::NetMessageType type) const
{
    if (!client_.is_connected()) {
        return false;
    }

    std::vector<std::byte> data(sizeof(std::uint32_t));
    const std::uint32_t message_type{ type };
    std::memcpy(data.data(), &message_type, sizeof(message_type));
    const auto* begin{ reinterpret_cast<const std::byte*>(text.data()) };
    data.insert(data.end(), begin, begin + text.size());
    data.push_back(std::byte{ 0 });
    return client_.write(data);
}

bool SessionHandler::send_to_client(const packet::Payload& payload) const
{
    auto data{ packet::PacketHelper::serialize(payload) };
    if (data.empty()) {
        return false;
    }
    data.push_back(std::byte{ 0 });
    return server_.write(data);
}

bool SessionHandler::send_to_server(const packet::Payload& payload) const
{
    auto data{ packet::PacketHelper::serialize(payload) };
    if (data.empty()) {
        return false;
    }
    data.push_back(std::byte{ 0 });
    return client_.write(data);
}

void SessionHandler::on_spawn(const event::Event& event)
{
    const auto evt{ as<packet::PacketId::OnSpawn>(event, event::Direction::ClientBound) };
    const auto pkt{ evt ? evt->get<packet::game::OnSpawn>() : nullptr };
    if (!pkt || pkt->type != "local") {
        return;
    }

    local_net_id_ = pkt->net_id;
    real_name_ = pkt->name;
    local_position_ = glm::vec2{ pkt->position.x, pkt->position.y };
}

void SessionHandler::on_world_select_menu(const event::Event& event)
{
    if (!as<packet::PacketId::OnRequestWorldSelectMenu>(event, event::Direction::ClientBound)) {
        return;
    }

    in_world_ = false;
    local_net_id_ = -1;
}

void SessionHandler::on_map_data(const event::Event& event)
{
    if (!as<packet::PacketId::SendMapData>(event, event::Direction::ClientBound)) {
        return;
    }

    std::string name{ world::World::instance().get_name() };
    if (!features::normalize_world_name(name)) {
        name = last_join_.substr(0, last_join_.find('|'));
    }

    in_world_ = true;
    local_net_id_ = -1;
    local_position_.reset();

    if (!name.empty() && name != current_world_) {
        if (!current_world_.empty()) {
            previous_world_ = current_world_;
        }
        current_world_ = name;
        std::erase(recent_worlds_, name);
        recent_worlds_.push_front(name);
        if (recent_worlds_.size() > 10) {
            recent_worlds_.pop_back();
        }
    }
}

void SessionHandler::on_join_request(const event::Event& event)
{
    const auto evt{ as<packet::PacketId::JoinRequest>(event, event::Direction::ServerBound) };
    if (const auto pkt{ evt ? evt->get<packet::message::JoinRequest>() : nullptr }) {
        if (const auto name{ features::normalize_world_name(pkt->world_name) }) {
            last_join_ = *name;
        }
    }
}

void SessionHandler::on_state(const event::Event& event)
{
    const auto evt{ as<packet::PacketId::State>(event, event::Direction::ServerBound) };
    if (const auto pkt{ evt ? evt->get<packet::game::State>() : nullptr }; pkt && in_world_) {
        local_position_ = glm::vec2{ pkt->game_packet.pos_x, pkt->game_packet.pos_y };
    }
}

void SessionHandler::on_disconnect()
{
    in_world_ = false;
    local_net_id_ = -1;
}
}
