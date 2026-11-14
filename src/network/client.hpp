#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <span>
#include <string>
#include <enet/enet.h>

#include "connection.hpp"
#include "enet_wrapper.hpp"
#include "../core/config.hpp"
#include "../event/event.hpp"
#include "../packet/packet_decoder.hpp"

namespace network {
class Client final : public ENetWrapper, public IConnection {
public:
    Client(core::Config& config, event::Dispatcher& dispatcher);

    bool connect(const std::string& host, std::uint16_t port);

    [[nodiscard]] bool write(std::span<const std::byte> data, int channel = 0) const override;
    [[nodiscard]] bool write(const std::vector<std::byte>& data, int channel = 0) const override;

    void disconnect() const;
    void disconnect_now();

    [[nodiscard]] bool is_connected() const override;
    // True while a connection is being established or is established.
    [[nodiscard]] bool is_active() const;
    // The last server connect() was called with, if any.
    [[nodiscard]] const std::optional<std::pair<std::string, std::uint16_t>>& last_target() const { return last_target_; }

    void flush() const;

    // Called from the main loop: warns when the Growtopia server does not
    // answer a connection attempt, with packet counters that tell "no reply at
    // all" (firewall / network / blocked) apart from a protocol problem.
    void check_connect_progress();

protected:
    void on_connect(ENetPeer* peer) override;
    void on_receive(ENetPeer* peer, std::span<const std::byte> data) override;
    void on_disconnect(ENetPeer* peer) override;

private:
    static ENetHost* create_host();

private:
    core::Config& config_;

    event::Dispatcher& dispatcher_;
    ENetPeer* peer_;
    std::optional<std::pair<std::string, std::uint16_t>> last_target_;

    bool connected_{ false };
    int connect_warnings_{ 0 };
    std::chrono::steady_clock::time_point connect_started_{};
    std::uint32_t sent_at_connect_{ 0 };
    std::uint32_t received_at_connect_{ 0 };
};
}
