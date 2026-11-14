#include "connection_handler.hpp"

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <system_error>
#include <vector>

#include "../../packet/game/server.hpp"
#include "../../packet/game/item_database.hpp"

namespace core::handlers {
namespace {
// Locations where an installed Growtopia client keeps its items.dat cache.
std::vector<std::filesystem::path> game_items_dat_candidates()
{
    std::vector<std::filesystem::path> paths;

    if (const char* local_app_data{ std::getenv("LOCALAPPDATA") }) {
        paths.emplace_back(std::filesystem::path{ local_app_data } / "Growtopia" / "cache" / "items.dat");
    }

    if (const char* home{ std::getenv("HOME") }) {
        paths.emplace_back(
            std::filesystem::path{ home } / "Library" / "Application Support" / "growtopia" / "cache" / "items.dat"
        );
    }

    return paths;
}

std::optional<std::filesystem::path> find_game_items_dat(const std::uint32_t expected_hash)
{
    for (const auto& path : game_items_dat_candidates()) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) {
            continue;
        }

        const auto hash{ static_cast<std::uint32_t>(utils::hash::proton_file(path.string())) };
        if (hash == expected_hash) {
            return path;
        }

        spdlog::debug("Game items.dat at {} has hash {}, expected {}", path.string(), hash, expected_hash);
    }

    return std::nullopt;
}
}

ConnectionHandler::ConnectionHandler(
    event::Dispatcher& dispatcher,
    network::Client& client,
    network::Server& server,
    Config& config
)
    : dispatcher_{ dispatcher }
    , client_{ client }
    , server_{ server }
    , config_{ config }
{
    setup_connection_handlers();
    setup_on_send_to_server_handler();
    setup_quit_handler();
    setup_disconnect_handler();
    setup_send_item_database_data_handler();
    setup_on_super_main_start_handler();
}

void ConnectionHandler::setup_connection_handlers()
{
    handles_.emplace_back(
        dispatcher_,
        event::Type::ClientConnect,
        dispatcher_.append_listener(event::Type::ClientConnect, [this](const event::Event&) {
            if (pending_port_ == 65535) {
                return;
            }

            spdlog::info("Connecting to Growtopia server at {}:{}", pending_address_, pending_port_);
            client_.connect(pending_address_, pending_port_);

            pending_address_.clear();
            pending_port_ = 65535;
        })
    );

    // Runs after every other ClientConnect listener. If neither server_data.php
    // nor a sub-server redirect told us where to go (e.g. the game reconnected
    // after a network drop), reconnect to the last server instead of leaving the
    // client hanging.
    handles_.emplace_back(
        dispatcher_,
        event::Type::ClientConnect,
        dispatcher_.append_listener(event::Type::ClientConnect, [this](const event::Event&) {
            if (client_.is_active()) {
                return;
            }

            const auto& last{ client_.last_target() };
            if (!last) {
                spdlog::error(
                    "Game connected but no Growtopia server is known yet "
                    "(server_data.php did not go through the proxy - check the hosts file)"
                );
                return;
            }

            spdlog::warn("No new server_data.php or redirect; reconnecting to last server {}:{}", last->first, last->second);
            client_.connect(last->first, last->second);
        }, event::Priority::Lowest)
    );

    handles_.emplace_back(
        dispatcher_,
        event::Type::ClientDisconnect,
        dispatcher_.append_listener(event::Type::ClientDisconnect, [this](const event::Event&) {
            if (!client_.is_active()) {
                return;
            }

            // Close the upstream connection at once: the game usually reconnects
            // within milliseconds (sub-server switch) and the proxy's single
            // upstream slot must be free by then. A graceful disconnect could sit
            // behind unacknowledged data and never be sent; disconnect_now() drops
            // the queue and notifies the server immediately.
            client_.disconnect_now();
            spdlog::info("Game client left; closed the connection to the Growtopia server");
        })
    );

    handles_.emplace_back(
        dispatcher_,
        event::Type::ServerDisconnect,
        dispatcher_.append_listener(event::Type::ServerDisconnect, [this](const event::Event&) {
            if (!server_.is_connected()) {
                return;
            }

            server_.disconnect();
            spdlog::info("Gracefully disconnect Growtopia client from proxy server");
        })
    );
}

void ConnectionHandler::setup_on_send_to_server_handler()
{
    constexpr auto on_send_to_server_type = event::packet_event_type(packet::PacketId::OnSendToServer);
    handles_.emplace_back(
        dispatcher_,
        on_send_to_server_type,
        dispatcher_.append_listener(on_send_to_server_type, [this](const event::Event& event) {
            const auto* evt = dynamic_cast<const event::TypedPacketEvent<packet::PacketId::OnSendToServer>*>(&event);
            if (!evt) {
                return;
            }

            const auto pkt{ evt->get<packet::game::OnSendToServer>() };
            if (!pkt) {
                return;
            }

            pending_address_ = pkt->address;
            pending_port_ = pkt->port;

            const auto modified_pkt = std::make_shared<packet::game::OnSendToServer>(*pkt);
            modified_pkt->address = "127.0.0.1";
            modified_pkt->port = config_.get_server_config().port;

            std::ignore = packet::PacketHelper::write(*modified_pkt, server_);
            evt->cancel();
        })
    );
}

void ConnectionHandler::setup_quit_handler()
{
    constexpr auto quit_type{ event::packet_event_type(packet::PacketId::Quit) };
    handles_.emplace_back(
        dispatcher_,
        quit_type,
        dispatcher_.append_listener(quit_type, [this](const event::Event& event) {
            if (
                const auto evt{ dynamic_cast<const event::TypedPacketEvent<packet::PacketId::Quit>*>(&event) };
                !evt || evt->direction != event::Direction::ServerBound
            ) {
                return;
            }

            server_.disconnect();
            client_.disconnect_now();
            spdlog::info("Forced disconnect proxy client from Growtopia server");
        })
    );
}

void ConnectionHandler::setup_disconnect_handler()
{
    constexpr auto disconnect_type{ event::packet_event_type(packet::PacketId::Disconnect) };
    handles_.emplace_back(
        dispatcher_,
        disconnect_type,
        dispatcher_.append_listener(disconnect_type, [this](const event::Event& event) {
            if (
                const auto evt{ dynamic_cast<const event::TypedPacketEvent<packet::PacketId::Disconnect>*>(&event) };
                !evt || evt->direction != event::Direction::ServerBound
            ) {
                return;
            }

            server_.disconnect_now();
            spdlog::info("Forced disconnect proxy server from Growtopia client");
            client_.disconnect_now();
            spdlog::info("Forced disconnect proxy client from Growtopia server");
        })
    );
}

void ConnectionHandler::setup_send_item_database_data_handler()
{
    constexpr auto send_item_database_data_type{ event::packet_event_type(packet::PacketId::SendItemDatabaseData) };
    handles_.emplace_back(
        dispatcher_,
        send_item_database_data_type,
        dispatcher_.append_listener(send_item_database_data_type, [](const event::Event& event) {
            const auto evt{ dynamic_cast<const event::TypedPacketEvent<packet::PacketId::SendItemDatabaseData>*>(&event) };
            if (!evt || evt->direction != event::Direction::ClientBound) {
                return;
            }

            const auto pkt{ evt->get<packet::game::SendItemDatabaseData>() };
            if (!pkt || pkt->items_dat.empty()) {
                spdlog::warn("No data to parse");
                return;
            }

            const auto parsed{ item::ItemDatabase::instance().parse(pkt->items_dat) };
            if (!parsed) {
                spdlog::error(
                    "Failed to parse items.dat (version {}, count {})",
                    item::ItemDatabase::instance().get_version(),
                    item::ItemDatabase::instance().get_count()
                );
                return;
            }

            spdlog::info(
                "Successfully parsed items.dat (version {}, count {})",
                item::ItemDatabase::instance().get_version(),
                item::ItemDatabase::instance().get_count()
            );

            // ReSharper disable once CppVariableCanBeMadeConstexpr
            const std::string cache_path{ "resources/items.dat" };
            try {
                std::ofstream out{ cache_path, std::ios::binary };
                if (!out) {
                    spdlog::warn("Failed to open items.dat for writing: {}", cache_path);
                    return;
                }

                out.write(reinterpret_cast<const char*>(pkt->items_dat.data()), pkt->items_dat.size());
                spdlog::info("Saved items.dat to {} ({} bytes)", cache_path, pkt->items_dat.size());
            } catch (const std::exception& e) {
                spdlog::error("Failed to save items.dat: {}", e.what());
            }
        })
    );
}

void ConnectionHandler::setup_on_super_main_start_handler()
{
    constexpr auto on_send_to_server_type = event::packet_event_type(packet::PacketId::OnSuperMainStartAcceptLogonHrdxs47254722215a);
    handles_.emplace_back(
        dispatcher_,
        on_send_to_server_type,
        dispatcher_.append_listener(on_send_to_server_type, [this](const event::Event& event) {
            const auto* evt = dynamic_cast<const event::TypedPacketEvent<packet::PacketId::OnSuperMainStartAcceptLogonHrdxs47254722215a>*>(&event);
            if (!evt) {
                return;
            }

            const auto pkt{ evt->get<packet::game::OnSuperMainStartAcceptLogonHrdxs47254722215a>() };
            if (!pkt) {
                return;
            }

            // ReSharper disable once CppVariableCanBeMadeConstexpr
            const std::string cache_path{ "resources/items.dat" };
            const auto server_hash{ static_cast<std::uint32_t>(pkt->item_hash) };
            const auto cached_hash{ static_cast<std::uint32_t>(utils::hash::proton_file(cache_path)) };

            std::string load_path{ cache_path };
            if (server_hash != cached_hash) {
                spdlog::info("items.dat hash mismatch: server={}, cached={}", server_hash, cached_hash);

                // The server only sends items.dat when the game client's own copy is
                // outdated. If the client is already up to date, take its copy instead.
                const auto game_path{ find_game_items_dat(server_hash) };
                if (!game_path) {
                    // Tell the client its items.dat is outdated (hash 0) so it asks the
                    // server for a fresh copy, which the SendItemDatabaseData handler
                    // then parses and saves. Costs one download per items.dat update.
                    spdlog::info("No matching items.dat available; asking the client to download it from the server");

                    const auto patched{ std::make_shared<packet::game::OnSuperMainStartAcceptLogonHrdxs47254722215a>(*pkt) };
                    patched->item_hash = 0;
                    if (packet::PacketHelper::write(*patched, server_)) {
                        evt->cancel();
                    } else {
                        spdlog::warn("Failed to send the patched logon packet; forwarding the original");
                    }
                    return;
                }

                load_path = game_path->string();
                spdlog::info("Using game client's items.dat from {}", load_path);

                std::error_code ec;
                std::filesystem::copy_file(*game_path, cache_path, std::filesystem::copy_options::overwrite_existing, ec);
                if (ec) {
                    spdlog::warn("Failed to copy items.dat to {}: {}", cache_path, ec.message());
                }
            } else {
                spdlog::info("Hash matches, loading cached items.dat (hash: {})", cached_hash);
            }

            if (!item::ItemDatabase::instance().load_from_file(load_path)) {
                spdlog::warn("Failed to load items.dat from {}", load_path);
                return;
            }

            spdlog::info(
                "Successfully loaded cached items.dat (version {}, count {})",
                item::ItemDatabase::instance().get_version(),
                item::ItemDatabase::instance().get_count()
            );
        })
    );
}

} // namespace core
