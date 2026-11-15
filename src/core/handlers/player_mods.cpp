#include "player_mods.hpp"

#include <spdlog/spdlog.h>

#include "../../packet/game/player.hpp"
#include "../../packet/packet_types.hpp"

namespace core::handlers {
using namespace player_state;

PlayerMods::PlayerMods(event::Dispatcher& dispatcher, network::Client& client, network::Server& server, SessionHandler& session)
    : dispatcher_{ dispatcher }
    , client_{ client }
    , server_{ server }
    , session_{ session }
{
    // Before the packets are passed on (forwarding listens at Normal).
    handles_.emplace_back(dispatcher_, event::Type::ClientBoundPacket, dispatcher_.append_listener(
        event::Type::ClientBoundPacket, [this](const event::Event& e) { on_from_server(e); }, event::Priority::Highest
    ));
    handles_.emplace_back(dispatcher_, event::Type::ServerBoundPacket, dispatcher_.append_listener(
        event::Type::ServerBoundPacket, [this](const event::Event& e) { on_from_game(e); }, event::Priority::Highest
    ));
    for (const auto type : { event::Type::ClientDisconnect, event::Type::ServerDisconnect }) {
        handles_.emplace_back(dispatcher_, type, dispatcher_.append_listener(type, [this](const event::Event&) {
            last_state_.clear();
            made_ = false;
            safe_position_.reset();
        }));
    }
}

bool PlayerMods::official() const
{
    return judge(client_.last_target()).official;
}

PlayerMods::Status PlayerMods::status() const
{
    Status status{};
    const auto verdict{ judge(client_.last_target()) };
    status.allowed = !verdict.official;
    status.why = verdict.why;
    status.game_server = verdict.game_server;
    status.state_known = !last_state_.empty() && !made_;
    status.state_made = made_;
    status.settings = settings_;
    return status;
}

PlayerMods::Status PlayerMods::set(const Settings& wanted)
{
    const auto settings{ clamped(wanted) };
    const bool changed{ settings != settings_ };
    settings_ = settings;

    auto now{ status() };
    if (!now.allowed) {
        spdlog::warn("Player abilities refused: {}", now.why);
        return now;
    }
    if (changed) {
        spdlog::info(
            "Player abilities: speed x{}, gravity x{}, acceleration x{}, water x{}, double jump {}, through blocks {}, range +{}, god mode {}",
            settings_.speed, settings_.gravity, settings_.acceleration, settings_.water_speed,
            settings_.double_jump ? "on" : "off", settings_.noclip ? "on" : "off", settings_.range, settings_.god ? "on" : "off"
        );
        send_state();
    }
    return status();
}

void PlayerMods::send_state()
{
    const auto net_id{ session_.local_net_id() };
    if (!session_.in_world() || net_id < 0 || !server_.is_connected()) {
        return;     // applied to the next state the server sends
    }
    if (last_state_.empty()) {
        if (!settings_.changes_state()) {
            return;
        }
        last_state_ = usual_state(net_id);
        made_ = true;
        spdlog::info("Player abilities: the server sent no character state yet; using the usual values");
    }
    std::ignore = server_.write(settings_.changes_state() ? player_state::apply(last_state_, settings_) : last_state_);
}

void PlayerMods::on_from_server(const event::Event& event)
{
    const auto* raw{ dynamic_cast<const event::RawPacketEvent*>(&event) };
    const auto header{ raw ? game_header(raw->data) : std::nullopt };
    if (!header || header->type != packet::PACKET_SET_CHARACTER_STATE) {
        return;
    }
    const auto net_id{ session_.local_net_id() };
    if (net_id < 0 || static_cast<std::int32_t>(header->net_id) != net_id) {
        return;
    }
    last_state_.assign(raw->data.begin(), raw->data.end());
    made_ = false;
    if (!settings_.changes_state() || official()) {
        return;
    }
    event.cancel();
    std::ignore = server_.write(player_state::apply(last_state_, settings_));
}

void PlayerMods::on_from_game(const event::Event& event)
{
    const auto* raw{ dynamic_cast<const event::RawPacketEvent*>(&event) };
    if (!raw) {
        return;
    }

    if (const auto header{ game_header(raw->data) }; header && header->type == packet::PACKET_STATE) {
        const auto flags{ static_cast<std::uint32_t>(header->flags.value) };
        if ((flags & HURT) == 0) {
            safe_position_ = glm::vec2{ header->pos_x, header->pos_y };
        }
        if (!settings_.god || official()) {
            return;
        }
        if (const auto healed{ without_damage(raw->data) }) {
            event.cancel();
            std::ignore = client_.write(*healed);
        }
        return;
    }

    // A death on spikes and the like: the game asks to be respawned.
    if (!settings_.god || message_type(raw->data) != packet::NET_MESSAGE_GENERIC_TEXT) {
        return;
    }
    const std::string_view text{ reinterpret_cast<const char*>(raw->data.data()) + sizeof(std::uint32_t), raw->data.size() - sizeof(std::uint32_t) };
    if (!text.starts_with("action|respawn_spike") || official()) {
        return;
    }
    event.cancel();
    spdlog::info("God mode: a death on spikes was not reported to the server");
    if (safe_position_ && session_.local_net_id() >= 0) {
        packet::game::OnSetPos back{};
        back.net_id = session_.local_net_id();
        back.position = *safe_position_;
        std::ignore = session_.send_to_client(back.write());
    }
}
}
