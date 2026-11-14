#include "client.hpp"

#include <chrono>
#include <stdexcept>
#include <string>
#include <spdlog/spdlog.h>

#include "../packet/packet_event_registry.hpp"
#include "../utils/network.hpp"

namespace network {
Client::Client(core::Config& config, event::Dispatcher& dispatcher)
    : ENetWrapper{ create_host() }
    , config_{ config }
    , dispatcher_{ dispatcher }
    , peer_{ nullptr }
{
    if (!host_) {
        throw std::runtime_error{ "Failed to create proxy client host" };
    }

    spdlog::info("Proxy client ready to connect");
}

ENetHost* Client::create_host()
{
    // Exactly one peer slot: the ENet fork derives the "new packet" header
    // integrity from the first peer's port, so outgoing connections must always
    // use slot 0. connect() frees the slot before every new connection.
    ENetHost* host{ enet_host_create(nullptr, 1, 2, 0, 0) };
    if (!host) {
        return nullptr;
    }

    if (enet_host_compress_with_range_coder(host) != 0) {
        enet_host_destroy(host);
        return nullptr;
    }

    host->checksum = enet_crc32;
    host->usingNewPacket = 1;

    return host;
}

bool Client::connect(const std::string& host, std::uint16_t port)
{
    if (!host_) {
        return false;
    }

    // When the game switches sub-server it reconnects immediately, while the
    // previous upstream connection may still be finishing a graceful disconnect.
    // Close it right away (the old server is still notified) so the single peer
    // slot is free; otherwise the new connection cannot be created.
    // Flush first: a queued graceful disconnect would otherwise never be sent,
    // because disconnect_now() sends nothing for a peer already disconnecting.
    enet_host_flush(host_);
    for (std::size_t i = 0; i < host_->peerCount; ++i) {
        if (ENetPeer* old_peer{ &host_->peers[i] }; old_peer->state != ENET_PEER_STATE_DISCONNECTED) {
            enet_peer_disconnect_now(old_peer, 0);
        }
    }
    peer_ = nullptr;

    ENetAddress address{};
    if (enet_address_set_host(&address, host.c_str()) != 0) {
        spdlog::error("Failed to resolve Growtopia server address '{}'", host);
        peer_ = nullptr;
        return false;
    }
    address.port = port;

    last_target_ = std::make_pair(host, port);
    connected_ = false;
    connect_warnings_ = 0;
    connect_started_ = std::chrono::steady_clock::now();
    sent_at_connect_ = host_->totalSentPackets;
    received_at_connect_ = host_->totalReceivedPackets;

    peer_ = enet_host_connect(host_, &address, 2, 0);
    if (!peer_) {
        spdlog::error("Failed to start connection to Growtopia server {}:{} (no free peer)", host, port);
        return false;
    }

    return true;
}

void Client::on_connect(ENetPeer* peer)
{
    const auto elapsed{ std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - connect_started_
    ) };
    spdlog::info(
        "Proxy client connected to Growtopia server at {}:{} (in {} ms)",
        utils::network::format_ip_address(peer->address.host),
        peer->address.port,
        elapsed.count()
    );

    peer_ = peer;
    connected_ = true;

    const event::ConnectionEvent evt{ event::Type::ServerConnect };
    dispatcher_.dispatch(evt);
}

void Client::on_receive(ENetPeer* peer, const std::span<const std::byte> data)
{
    if (peer != peer_) {
        return;
    }

    if (data.size() < 4) {
        spdlog::warn("Received malformed packet from Growtopia server (size {})", data.size());
        disconnect();
        return;
    }

    const auto decoded{ packet::PacketDecoder::decode(data, config_.get_log_config(), "ClientBound") };
    if (!decoded.has_value()) {
        const event::RawPacketEvent evt{ event::Type::ClientBoundPacket, data };
        dispatcher_.dispatch(evt);
        return;
    }

    const auto& packet{ decoded.value() };
    if (
        const auto& registry{ packet::event_registry::PacketEventRegistry::instance() };
        registry.has_event(packet->id())
    ) {
        const auto evt{ registry.emit(
            dispatcher_,
            event::Direction::ClientBound,
            packet
        ) };

        if (evt && evt->canceled) {
            return;
        }
    }

    const event::RawPacketEvent evt{ event::Type::ClientBoundPacket, data };
    dispatcher_.dispatch(evt);
}

void Client::on_disconnect(ENetPeer* peer)
{
    if (peer != peer_) {
        return;
    }

    if (!connected_) {
        spdlog::error(
            "Could not connect to Growtopia server {}:{}: timed out (sent {} UDP packets, received {})",
            last_target_ ? last_target_->first : std::string{ "?" },
            last_target_ ? last_target_->second : 0,
            host_->totalSentPackets - sent_at_connect_,
            host_->totalReceivedPackets - received_at_connect_
        );
    } else {
        spdlog::info("Growtopia server closed the connection");
    }
    connected_ = false;

    peer_ = nullptr;

    const event::ConnectionEvent evt{ event::Type::ServerDisconnect };
    dispatcher_.dispatch(evt);
}

bool Client::write(std::span<const std::byte> data, const int channel) const
{
    if (!is_connected()) {
        return false;
    }

    ENetPacket* packet{ enet_packet_create(
        data.data(),
        data.size(),
        ENET_PACKET_FLAG_RELIABLE
    ) };

    if (enet_peer_send(peer_, channel, packet) != 0) {
        enet_packet_destroy(packet);
        return false;
    }

    return true;
}

bool Client::write(const std::vector<std::byte>& data, const int channel) const
{
    return write(std::span{ data.data(), data.size() }, channel);
}

void Client::disconnect() const
{
    if (peer_) {
        enet_peer_disconnect(peer_, 0);
    }
}

void Client::disconnect_now()
{
    if (!peer_) {
        return;
    }

    if (!connected_ && is_active()) {
        spdlog::warn(
            "Gave up connecting to Growtopia server {}:{} after {} s (sent {} UDP packets, received {})",
            last_target_ ? last_target_->first : std::string{ "?" },
            last_target_ ? last_target_->second : 0,
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - connect_started_).count(),
            host_->totalSentPackets - sent_at_connect_,
            host_->totalReceivedPackets - received_at_connect_
        );
    }

    enet_peer_disconnect_now(peer_, 0);
    peer_ = nullptr;
    connected_ = false;
}

void Client::check_connect_progress()
{
    if (!peer_ || connected_ || !is_active() || connect_warnings_ >= 3) {
        return;
    }

    const auto elapsed{ std::chrono::steady_clock::now() - connect_started_ };
    const auto due{ std::chrono::seconds{ 5 } * (connect_warnings_ + 1) };
    if (elapsed < due) {
        return;
    }

    ++connect_warnings_;
    const auto sent{ host_->totalSentPackets - sent_at_connect_ };
    const auto received{ host_->totalReceivedPackets - received_at_connect_ };
    spdlog::warn(
        "Still connecting to Growtopia server {}:{} after {} s: sent {} UDP packets, received {}{}",
        last_target_ ? last_target_->first : std::string{ "?" },
        last_target_ ? last_target_->second : 0,
        std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(),
        sent,
        received,
        received == 0
            ? " - the server does not answer: check that Windows Firewall allows Proxy.exe "
              "(Private and Public), antivirus/VPN, or whether this IP is blocked"
            : " - the server answers but does not accept the connection (protocol mismatch?)"
    );
}

bool Client::is_active() const
{
    if (!peer_) {
        return false;
    }

    switch (peer_->state) {
    case ENET_PEER_STATE_CONNECTING:
    case ENET_PEER_STATE_ACKNOWLEDGING_CONNECT:
    case ENET_PEER_STATE_CONNECTION_PENDING:
    case ENET_PEER_STATE_CONNECTION_SUCCEEDED:
    case ENET_PEER_STATE_CONNECTED:
        return true;
    default:
        return false;
    }
}

bool Client::is_connected() const
{
    return peer_ && peer_->state == ENET_PEER_STATE_CONNECTED;
}

void Client::flush() const
{
    if (host_) {
        enet_host_flush(host_);
    }
}
}
