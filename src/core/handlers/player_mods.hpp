#pragma once
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "player_state.hpp"
#include "session_handler.hpp"
#include "../../event/event.hpp"
#include "../../network/client.hpp"
#include "../../network/server.hpp"

namespace core::handlers {
// Your own character's abilities on your own server, set the way the server
// sets them for items (wings, speedy shoes ...): the server's character
// state (PACKET_SET_CHARACTER_STATE: walking speed, gravity, acceleration,
// double jump, walking through blocks, punch / build range) reaches the game
// with your settings applied, and is sent again when they change.
//
// God mode: the damage the game reports to the server (lava, acid, hits in
// the state reports) is not passed on, and a death on spikes is not
// reported: the proxy puts you back where you stood instead.
//
// Never on Growtopia's own servers: there these are cheats against other
// players. Decided by the game server the proxy is connected to (see
// player_state::judge), not by the name in config.json.
class PlayerMods {
public:
    using Settings = player_state::Abilities;

    struct Status {
        bool allowed{ false };          // false on Growtopia's own servers
        std::string why;
        std::string game_server;        // "host:port" the proxy plays on
        bool state_known{ false };      // the server's character state was seen
        bool state_made{ false };       // none came: the proxy made one from the usual values
        Settings settings;
    };

    PlayerMods(event::Dispatcher& dispatcher, network::Client& client, network::Server& server, SessionHandler& session);

    PlayerMods(const PlayerMods&) = delete;
    PlayerMods& operator=(const PlayerMods&) = delete;
    ~PlayerMods() = default;

    // Clamps the values, keeps them and sends the game its state again.
    Status set(const Settings& settings);
    [[nodiscard]] Status status() const;
    [[nodiscard]] bool official() const;

private:
    void on_from_server(const event::Event& event);
    void on_from_game(const event::Event& event);
    void send_state();

    event::Dispatcher& dispatcher_;
    network::Client& client_;
    network::Server& server_;
    SessionHandler& session_;
    std::vector<event::ScopedHandle> handles_;

    Settings settings_{};
    std::vector<std::byte> last_state_;     // what the server sent last (unchanged)
    bool made_{ false };
    std::optional<glm::vec2> safe_position_;
};
}
