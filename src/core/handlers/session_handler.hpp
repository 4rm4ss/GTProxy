#pragma once
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "../../event/event.hpp"
#include "../../network/client.hpp"
#include "../../network/server.hpp"
#include "../../packet/packet_types.hpp"
#include "../../packet/payload.hpp"

namespace core::handlers {
// Where you are in the game, as far as the proxy can tell: connected or not,
// the world you are in and the ones before, your netID and position. It
// only follows the packets and offers ways to talk to the game and the
// server; features built on it live in modules/.
class SessionHandler {
public:
    SessionHandler(event::Dispatcher& dispatcher, network::Server& server, network::Client& client);

    SessionHandler(const SessionHandler&) = delete;
    SessionHandler& operator=(const SessionHandler&) = delete;
    ~SessionHandler() = default;

    [[nodiscard]] bool connected() const { return client_.is_connected(); }
    [[nodiscard]] bool in_world() const { return in_world_; }
    [[nodiscard]] const std::string& current_world() const { return current_world_; }
    [[nodiscard]] const std::string& previous_world() const { return previous_world_; }
    // Worlds entered this session, newest first (at most 10).
    [[nodiscard]] const std::deque<std::string>& recent_worlds() const { return recent_worlds_; }
    // Your name as the server sends it (with colour codes).
    [[nodiscard]] const std::string& real_name() const { return real_name_; }
    [[nodiscard]] std::int32_t local_net_id() const { return local_net_id_; }
    // Your position in world pixels (32 per tile), once known.
    [[nodiscard]] std::optional<glm::vec2> local_position() const { return local_position_; }

    // Shows text in the game's console / chat.
    void message(const std::string& text) const;
    // Opens a dialog (OnDialogRequest markup) in the game.
    void show_dialog(const std::string& markup) const;
    // Sends text to the game server as the game would: GENERIC_TEXT for
    // actions such as "action|drop", GAME_MESSAGE for "action|join_request".
    bool send_text_to_server(const std::string& text, packet::NetMessageType type = packet::NET_MESSAGE_GENERIC_TEXT) const;
    bool send_to_client(const packet::Payload& payload) const;
    bool send_to_server(const packet::Payload& payload) const;

private:
    void on_spawn(const event::Event& event);
    void on_world_select_menu(const event::Event& event);
    void on_map_data(const event::Event& event);
    void on_join_request(const event::Event& event);
    void on_state(const event::Event& event);
    void on_disconnect();

    event::Dispatcher& dispatcher_;
    network::Server& server_;
    network::Client& client_;
    std::vector<event::ScopedHandle> handles_;

    std::int32_t local_net_id_{ -1 };
    std::string real_name_;
    bool in_world_{ false };
    std::string current_world_;
    std::string previous_world_;
    std::string last_join_;
    std::deque<std::string> recent_worlds_;
    std::optional<glm::vec2> local_position_;
};
}
