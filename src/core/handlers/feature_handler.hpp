#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "../../event/event.hpp"
#include "../../network/client.hpp"
#include "../../network/server.hpp"
#include "../../packet/payload.hpp"

namespace core::handlers {
// State and packet handling behind the proxy's own features: /warp, /back,
// /nick, showing netIDs and the /proxy menu. Everything here only changes
// what this game client shows, except /warp, /back and /rejoin, which send
// the same packets the game sends when you exit and type a world name.
class FeatureHandler {
public:
    FeatureHandler(event::Dispatcher& dispatcher, network::Server& server, network::Client& client);

    FeatureHandler(const FeatureHandler&) = delete;
    FeatureHandler& operator=(const FeatureHandler&) = delete;
    ~FeatureHandler() = default;

    // Joins "WORLD" or "WORLD|DOOR". From inside a world it first exits and
    // joins once the server shows the world menu. Returns false if not
    // connected to the game server.
    bool warp(const std::string& target);

    void set_nick(const std::string& nick);
    void reset_nick();
    [[nodiscard]] const std::string& nick() const { return nick_; }

    void set_show_net_ids(bool show);
    [[nodiscard]] bool show_net_ids() const { return show_net_ids_; }

    [[nodiscard]] bool in_world() const { return in_world_; }
    [[nodiscard]] const std::string& current_world() const { return current_world_; }
    [[nodiscard]] const std::string& previous_world() const { return previous_world_; }
    [[nodiscard]] const std::string& real_name() const { return real_name_; }
    // Local player position in pixels (32 per tile), once known.
    [[nodiscard]] std::optional<glm::vec2> local_position() const { return local_position_; }

    // Shows text in the game's console / chat.
    void message(const std::string& text) const;
    // Opens a dialog (OnDialogRequest markup) in the game.
    void show_dialog(const std::string& markup) const;

    // Called from the main loop.
    void update();

private:
    struct KnownPlayer {
        std::string name;   // as the server sent it (with colour codes)
        std::string title;  // title/wrench JSON for OnNameChanged
    };

    void send_to_client(const packet::Payload& payload) const;
    void send_to_server(const packet::Payload& payload) const;
    void send_join(const std::string& target);
    void refresh_name(std::int32_t net_id);
    [[nodiscard]] std::string shown_name(std::int32_t net_id, const std::string& server_name) const;

    void on_spawn(const event::Event& event);
    void on_name_changed(const event::Event& event);
    void on_remove(const event::Event& event);
    void on_world_select_menu(const event::Event& event);
    void on_map_data(const event::Event& event);
    void on_join_request(const event::Event& event);
    void on_state(const event::Event& event);
    void on_disconnect();

    event::Dispatcher& dispatcher_;
    network::Server& server_;
    network::Client& client_;
    std::vector<event::ScopedHandle> handles_;

    std::string nick_;
    bool show_net_ids_{ false };
    std::unordered_map<std::int32_t, KnownPlayer> players_;
    std::int32_t local_net_id_{ -1 };
    std::string real_name_;

    bool in_world_{ false };
    std::string current_world_;
    std::string previous_world_;
    std::string last_join_;
    std::optional<glm::vec2> local_position_;

    std::string pending_warp_;
    std::chrono::steady_clock::time_point warp_deadline_{};
};
}
