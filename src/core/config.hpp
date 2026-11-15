#pragma once
#include <string>

namespace core {
class Config {
public:
    struct ServerConfig {
        int port{ 16999 };
        std::string address{ "www.growtopia1.com" };
    };

    struct ClientConfig {
        std::string game_version{ "5.39" };
        int protocol{ 225 };
        std::string dns_server{ "cloudflare" };
    };

    struct LogConfig {
        bool print_message{ true };
        bool print_game_update_packet{ false };
        bool print_variant{ true };
        bool print_extra{ true };
    };

    struct CommandConfig {
        char prefix{ '/' };
    };

    // The map window (http://127.0.0.1:<port>/ in a browser) and walking
    // to a clicked tile.
    struct MapConfig {
        bool enabled{ true };
        int port{ 17171 };
        // Open the window by itself when the proxy starts.
        bool open_on_start{ false };
        // Walking speed in world pixels per second (32 per tile).
        float walk_speed{ 250.0f };
        // How often a position is sent while walking.
        int step_interval_ms{ 100 };
        // false: paths go through any free tile; true: only paths you could
        // run and jump along, rising at most jump_height tiles per jump.
        bool gravity{ false };
        int jump_height{ 3 };
        // Walk through doors and portals that are open to you (public, in an
        // unlocked world, or under a lock you own or have access to). A door
        // with a destination still takes you there; the main door never.
        bool walk_through_doors{ true };
    };

    struct WrapperConfig {
        ServerConfig server;
        ClientConfig client;
        LogConfig log;
        CommandConfig command;
        MapConfig map;
    };

public:
    Config();
    ~Config() = default;

public:
    [[nodiscard]] const ServerConfig& get_server_config() const { return config_.server; }
    [[nodiscard]] const ClientConfig& get_client_config() const { return config_.client; }
    [[nodiscard]] const LogConfig& get_log_config() const { return config_.log; }
    [[nodiscard]] const CommandConfig& get_command_config() const { return config_.command; }
    [[nodiscard]] const MapConfig& get_map_config() const { return config_.map; }

private:
    WrapperConfig config_;
};
}
