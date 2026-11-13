#pragma once
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
};
}
