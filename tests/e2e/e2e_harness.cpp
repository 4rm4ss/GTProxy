// End-to-end harness: a fake Growtopia client and two fake game servers that
// talk to a running GTProxy over ENet, using the same ENet protocol flags as the
// game. Driven by tests/e2e/run_e2e.py, which also fakes server_data.php.
//
// Usage: GTProxy_e2e <items.dat path>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <deque>
#include <fstream>
#include <iterator>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <enet/enet.h>
#include <fmt/format.h>

#include "item/item_database.hpp"
#include "packet/packet_types.hpp"
#include "packet/packet_variant.hpp"
#include "utils/byte_stream.hpp"
#include "utils/hash.hpp"
#include "utils/zlib.hpp"

using Bytes = std::vector<std::byte>;
using namespace std::chrono_literals;

namespace {
constexpr std::uint16_t PROXY_PORT{ 16999 };
constexpr std::uint16_t GAME_SERVER_PORT{ 17091 };
constexpr std::uint16_t SUB_SERVER_PORT{ 17092 };
// The sub-server address the fake game server hands out. The proxy must
// connect here itself and tell the client to connect to 127.0.0.1 instead.
constexpr const char* SUB_SERVER_ADDRESS{ "127.0.0.3" };

int g_failures{ 0 };

void check(const bool ok, const std::string& what)
{
    fmt::print("[{}] {}\n", ok ? "PASS" : "FAIL", what);
    std::fflush(stdout);
    if (!ok) {
        ++g_failures;
    }
}

struct Endpoint {
    std::string name;
    ENetHost* host{ nullptr };
    ENetPeer* peer{ nullptr };
    bool connected{ false };
    int connects{ 0 };
    int disconnects{ 0 };
    std::deque<Bytes> inbox;
};

ENetHost* make_host(
    const std::optional<std::uint16_t> listen_port,
    const bool server_side,
    const char* bind_ip = "127.0.0.1"
) {
    // Bind to an exact address: ENet drops replies that come from a different
    // IP than the one it connected to.
    ENetAddress address{};
    enet_address_set_host_ip(&address, bind_ip);
    ENetHost* host{ nullptr };

    if (listen_port) {
        address.port = *listen_port;
        host = enet_host_create(&address, 4, 2, 0, 0);
    } else {
        host = enet_host_create(nullptr, 2, 2, 0, 0);
    }

    if (!host) {
        return nullptr;
    }

    enet_host_compress_with_range_coder(host);
    host->checksum = enet_crc32;
    if (server_side) {
        host->usingNewPacketForServer = 1;
    } else {
        host->usingNewPacket = 1;
    }

    return host;
}

void service(Endpoint& ep)
{
    ENetEvent event{};
    while (enet_host_service(ep.host, &event, 0) > 0) {
        switch (event.type) {
        case ENET_EVENT_TYPE_CONNECT:
            ep.peer = event.peer;
            ep.connected = true;
            ++ep.connects;
            break;
        case ENET_EVENT_TYPE_RECEIVE: {
            const auto* data{ reinterpret_cast<const std::byte*>(event.packet->data) };
            ep.inbox.emplace_back(data, data + event.packet->dataLength);
            enet_packet_destroy(event.packet);
            break;
        }
        case ENET_EVENT_TYPE_DISCONNECT:
            if (event.peer == ep.peer) {
                ep.connected = false;
                ep.peer = nullptr;
            }
            ++ep.disconnects;
            break;
        default:
            break;
        }
    }
}

std::vector<Endpoint*> g_endpoints;

bool pump_until(const std::function<bool()>& done, const std::chrono::milliseconds timeout = 5000ms)
{
    const auto deadline{ std::chrono::steady_clock::now() + timeout };
    while (std::chrono::steady_clock::now() < deadline) {
        for (auto* ep : g_endpoints) {
            service(*ep);
        }

        if (done()) {
            return true;
        }

        std::this_thread::sleep_for(1ms);
    }

    return done();
}

void pump_for(const std::chrono::milliseconds duration)
{
    pump_until([] { return false; }, duration);
}

bool send(Endpoint& ep, const Bytes& data)
{
    if (!ep.peer) {
        return false;
    }

    ENetPacket* packet{ enet_packet_create(data.data(), data.size(), ENET_PACKET_FLAG_RELIABLE) };
    return enet_peer_send(ep.peer, 0, packet) == 0;
}

std::optional<Bytes> receive(Endpoint& ep, const std::chrono::milliseconds timeout = 5000ms)
{
    if (!pump_until([&] { return !ep.inbox.empty(); }, timeout)) {
        return std::nullopt;
    }

    auto data{ std::move(ep.inbox.front()) };
    ep.inbox.pop_front();
    return data;
}

// Packets the proxy re-serializes get a trailing NUL; raw forwards are exact.
bool same_payload(const Bytes& sent, const Bytes& received)
{
    if (sent == received) {
        return true;
    }

    return received.size() == sent.size() + 1
        && received.back() == std::byte{ 0 }
        && std::equal(sent.begin(), sent.end(), received.begin());
}

Bytes text_packet(const packet::NetMessageType type, const std::string& text)
{
    utils::ByteStream<> bs{};
    bs.write(static_cast<std::uint32_t>(type));
    bs.write_data(text.data(), text.size());
    bs.write<std::uint8_t>(0);
    return bs.take_data();
}

Bytes game_packet(packet::GameUpdatePacket header, const Bytes& extra)
{
    if (!extra.empty()) {
        header.flags.extended = 1;
        header.data_size = static_cast<std::uint32_t>(extra.size());
    }

    utils::ByteStream<> bs{};
    bs.write(static_cast<std::uint32_t>(packet::NET_MESSAGE_GAME_PACKET));
    bs.write(header);
    bs.write_data(extra.data(), extra.size());
    return bs.take_data();
}

Bytes variant_packet(const packet::PacketVariant& variant)
{
    packet::GameUpdatePacket header{};
    header.type = packet::PACKET_CALL_FUNCTION;
    header.net_id = static_cast<std::uint32_t>(-1);
    return game_packet(header, variant.serialize());
}

std::optional<packet::PacketVariant> parse_variant(const Bytes& data)
{
    constexpr std::size_t header_size{ sizeof(std::uint32_t) + sizeof(packet::GameUpdatePacket) };
    if (data.size() < header_size) {
        return std::nullopt;
    }

    std::uint32_t type{};
    std::memcpy(&type, data.data(), sizeof(type));
    if (type != packet::NET_MESSAGE_GAME_PACKET) {
        return std::nullopt;
    }

    packet::GameUpdatePacket header{};
    std::memcpy(&header, data.data() + sizeof(type), sizeof(header));
    if (header.type != packet::PACKET_CALL_FUNCTION) {
        return std::nullopt;
    }

    packet::PacketVariant variant{};
    if (!variant.deserialize({ data.data() + header_size, data.size() - header_size })) {
        return std::nullopt;
    }

    return variant;
}

Bytes build_world(const std::uint16_t lock_item_id)
{
    utils::ByteStream<> bs{};
    bs.write<std::uint16_t>(0x18);              // world version
    bs.write<std::uint32_t>(0);                 // unknown
    bs.write(std::string{ "E2ETESTWORLD" });

    constexpr std::int32_t width{ 10 };
    constexpr std::int32_t height{ 6 };
    bs.write<std::int32_t>(width);
    bs.write<std::int32_t>(height);
    bs.write<std::uint32_t>(width * height);
    for (int i = 0; i < 5; ++i) {
        bs.write<std::uint8_t>(0);
    }

    for (int i = 0; i < width * height; ++i) {
        if (i == 0) {
            // World Lock tile with lock extra data, like every real locked world.
            bs.write<std::uint16_t>(lock_item_id);
            bs.write<std::uint16_t>(0);
            bs.write<std::uint16_t>(0);
            bs.write<std::uint16_t>(1);         // TileFlag::Extra
            bs.write<std::uint8_t>(3);          // TileExtra::Type::Lock
            bs.write<std::uint8_t>(0);          // unk
            bs.write<std::uint32_t>(4242);      // owner id
            bs.write<std::uint32_t>(2);         // access count
            bs.write<std::uint32_t>(111);
            bs.write<std::uint32_t>(222);
            for (int j = 0; j < 8; ++j) {
                bs.write<std::uint8_t>(0);
            }
            if (lock_item_id == 5814) {
                for (int j = 0; j < 16; ++j) {
                    bs.write<std::uint8_t>(0);
                }
            }
            continue;
        }

        bs.write<std::uint16_t>(i % 3 == 0 ? 2 : 0);   // dirt or blank
        bs.write<std::uint16_t>(14);                    // background
        bs.write<std::uint16_t>(0);
        bs.write<std::uint16_t>(0);
    }

    for (int i = 0; i < 12; ++i) {
        bs.write<std::uint8_t>(0);
    }
    bs.write<std::uint32_t>(2);                 // object count
    bs.write<std::uint32_t>(3);                 // last drop id
    for (std::uint32_t i = 1; i <= 2; ++i) {
        bs.write<std::uint16_t>(112);           // gems
        bs.write<float>(32.0f * static_cast<float>(i));
        bs.write<float>(64.0f);
        bs.write<std::uint8_t>(5);
        bs.write<std::uint8_t>(0);
        bs.write<std::uint32_t>(i);
    }

    // Real worlds carry more data (weather etc.) after the objects.
    for (int i = 0; i < 16; ++i) {
        bs.write<std::uint8_t>(0xAA);
    }

    return bs.take_data();
}

Bytes map_data_packet(const Bytes& world)
{
    packet::GameUpdatePacket header{};
    header.type = packet::PACKET_SEND_MAP_DATA;
    header.net_id = static_cast<std::uint32_t>(-1);
    return game_packet(header, world);
}
}

int main(const int argc, char** argv)
{
    if (argc < 2) {
        fmt::print("usage: {} <items.dat>\n", argv[0]);
        return 2;
    }

    const std::string items_dat_path{ argv[1] };

    if (enet_initialize() != 0) {
        fmt::print("Failed to initialize ENet\n");
        return 2;
    }

    // Find a lock item id from the same items.dat the proxy will use.
    auto& items{ item::ItemDatabase::instance() };
    if (!items.load_from_file(items_dat_path)) {
        fmt::print("Failed to load {}\n", items_dat_path);
        return 2;
    }

    std::uint16_t lock_item_id{ 242 };
    for (std::uint32_t id = 0; id < items.get_count(); ++id) {
        if (const auto* info{ items.get_item(id) }; info && info->item_type == item::ItemType::Lock) {
            lock_item_id = static_cast<std::uint16_t>(id);
            break;
        }
    }
    fmt::print("Using lock item {} ({})\n", lock_item_id, items.get_item(lock_item_id)->item_name);

    std::ifstream items_file{ items_dat_path, std::ios::binary };
    const std::string items_raw{ std::istreambuf_iterator<char>{ items_file }, {} };
    const Bytes items_dat{
        reinterpret_cast<const std::byte*>(items_raw.data()),
        reinterpret_cast<const std::byte*>(items_raw.data()) + items_raw.size()
    };
    const auto items_hash{ utils::hash::proton_file(items_dat_path) };

    Endpoint client{ "client", make_host(std::nullopt, false) };
    Endpoint game_server{ "game server", make_host(GAME_SERVER_PORT, true) };
    Endpoint sub_server{ "sub server", make_host(SUB_SERVER_PORT, true, SUB_SERVER_ADDRESS) };
    if (!client.host || !game_server.host || !sub_server.host) {
        fmt::print("Failed to create ENet hosts\n");
        return 2;
    }
    g_endpoints = { &client, &game_server, &sub_server };

    // 1. Client connects to the proxy; the proxy connects to the game server
    //    it learned from server_data.php.
    ENetAddress proxy_address{};
    enet_address_set_host(&proxy_address, "127.0.0.1");
    proxy_address.port = PROXY_PORT;
    client.peer = enet_host_connect(client.host, &proxy_address, 2, 0);

    check(
        pump_until([&] { return client.connected && game_server.connected; }),
        "client connects to proxy and proxy connects to game server from server_data.php"
    );
    if (!client.connected || !game_server.connected) {
        fmt::print("Cannot continue without a connection\n");
        return 1;
    }

    // 2. Server hello reaches the client unchanged.
    const Bytes hello{ std::byte{ 1 }, std::byte{ 0 }, std::byte{ 0 }, std::byte{ 0 } };
    send(game_server, hello);
    {
        const auto got{ receive(client) };
        check(got && same_payload(hello, *got), "server hello forwarded to client");
    }

    // 3. Login packet reaches the server unchanged.
    const Bytes login{ text_packet(
        packet::NET_MESSAGE_GENERIC_TEXT,
        "protocol|225\ngame_version|5.61\nplatformID|0,1,1\nltoken|e2e-token\nfz|0\n"
    ) };
    send(client, login);
    {
        const auto got{ receive(game_server) };
        check(got && same_payload(login, *got), "login packet forwarded to game server");
    }

    // 4. Logon accept with a *changed* name suffix and the items.dat hash.
    const packet::PacketVariant logon{
        std::string{ "OnSuperMainStartAcceptLogonE2ENEWSUFFIX" },
        static_cast<std::int32_t>(items_hash),
        std::string{ "ubistatic-a.akamaihd.net" },
        std::string{ "0098/e2e/cache/" },
        std::string{ "cc.cz.madkite.freedom org.aqua.gg idv.aqua.bulldog" },
        std::uint32_t{ 1234 }
    };
    const Bytes logon_packet{ variant_packet(logon) };
    send(game_server, logon_packet);
    {
        const auto got{ receive(client) };
        check(got && same_payload(logon_packet, *got), "OnSuperMainStartAcceptLogon (new suffix) forwarded unchanged");
    }

    // 5. Server sends items.dat (zlib) like it does for outdated clients.
    {
        Bytes compressed;
        utils::compress_zlib(items_dat, compressed);

        packet::GameUpdatePacket header{};
        header.type = packet::PACKET_SEND_ITEM_DATABASE_DATA;
        header.net_id = static_cast<std::uint32_t>(-1);
        header.decompressed_data_size = static_cast<std::uint32_t>(items_dat.size());
        const Bytes items_packet{ game_packet(header, compressed) };

        send(game_server, items_packet);
        const auto got{ receive(client, 15000ms) };
        check(
            got && same_payload(items_packet, *got),
            fmt::format("items.dat packet ({} bytes compressed) forwarded unchanged", compressed.size())
        );
    }

    // 6. Proxy commands (built-in and Lua) are answered by the proxy and never
    //    reach the server; the game's own commands still do.
    for (const std::string command : { "/proxyhelp", "/rainbow" }) {
        game_server.inbox.clear();
        send(client, text_packet(packet::NET_MESSAGE_GENERIC_TEXT, "action|input\n|text|" + command));
        const auto reply{ receive(client, 3000ms) };
        pump_for(300ms);
        check(reply.has_value(), fmt::format("proxy answers {} itself", command));
        check(game_server.inbox.empty(), fmt::format("{} is not leaked to the game server", command));
        client.inbox.clear();
        game_server.inbox.clear();
    }
    {
        const Bytes game_help{ text_packet(packet::NET_MESSAGE_GENERIC_TEXT, "action|input\n|text|/help") };
        send(client, game_help);
        const auto got{ receive(game_server) };
        check(got && same_payload(game_help, *got), "game's own /help is forwarded to the game server");
    }

    // 7. Normal chat still goes to the server.
    {
        const Bytes chat{ text_packet(packet::NET_MESSAGE_GENERIC_TEXT, "action|input\n|text|hello world") };
        send(client, chat);
        const auto got{ receive(game_server) };
        check(got && same_payload(chat, *got), "normal chat forwarded to game server");
    }

    // 8. Join a world: valid world data is forwarded and parsed.
    {
        const Bytes join{ text_packet(packet::NET_MESSAGE_GAME_MESSAGE, "action|join_request\nname|E2ETESTWORLD\ninvitedWorld|0\n") };
        send(client, join);
        const auto got_join{ receive(game_server) };
        check(got_join && same_payload(join, *got_join), "join_request forwarded to game server");

        const Bytes world_packet{ map_data_packet(build_world(lock_item_id)) };
        send(game_server, world_packet);
        const auto got{ receive(client) };
        check(got && same_payload(world_packet, *got), "valid world data forwarded unchanged");
    }

    // 9. Garbage / truncated / hostile world data must not crash the proxy.
    {
        std::mt19937 rng{ 1337 };
        int forwarded{ 0 };
        constexpr int rounds{ 25 };
        for (int round = 0; round < rounds; ++round) {
            Bytes world{ build_world(lock_item_id) };
            if (round % 3 == 0) {
                world.resize(rng() % world.size());                     // truncated
            } else if (round % 3 == 1) {
                for (int k = 0; k < 20; ++k) {                          // corrupted
                    world[rng() % world.size()] = static_cast<std::byte>(rng());
                }
            } else {
                world.resize(64 + rng() % 512);                          // random
                for (auto& b : world) {
                    b = static_cast<std::byte>(rng());
                }
            }

            if (world.empty()) {
                world.push_back(std::byte{ 0 });
            }

            const Bytes world_packet{ map_data_packet(world) };
            send(game_server, world_packet);
            if (const auto got{ receive(client) }; got && same_payload(world_packet, *got)) {
                ++forwarded;
            }
        }
        check(forwarded == rounds, fmt::format("{}/{} malformed world packets forwarded, proxy alive", forwarded, rounds));
    }

    // 10. Sub-server redirect: proxy must rewrite the address to itself.
    const packet::PacketVariant send_to_server{
        std::string{ "OnSendToServer" },
        std::int32_t{ SUB_SERVER_PORT },
        std::int32_t{ 777001 },
        std::int32_t{ 555 },
        std::string{ fmt::format("{}|0|e2e-uuid-token", SUB_SERVER_ADDRESS) },
        std::int32_t{ 1 },
        std::string{ "E2EPlayer" }
    };
    send(game_server, variant_packet(send_to_server));
    {
        const auto got{ receive(client) };
        const auto variant{ got ? parse_variant(*got) : std::nullopt };
        check(variant.has_value(), "OnSendToServer reaches client");
        if (variant) {
            const auto& args{ variant->get_variants() };
            const bool shape_ok{ args.size() == 7 };
            check(shape_ok, fmt::format("OnSendToServer keeps all {} arguments", args.size()));
            if (shape_ok) {
                check(variant->get<std::int32_t>(1) == PROXY_PORT, "port rewritten to proxy port 16999");
                check(
                    variant->get<std::string>(4) == "127.0.0.1|0|e2e-uuid-token",
                    fmt::format("address rewritten to 127.0.0.1 ({})", variant->get<std::string>(4))
                );
                check(variant->get<std::int32_t>(2) == 777001, "token unchanged");
                check(variant->get<std::int32_t>(3) == 555, "user id unchanged");
                check(std::holds_alternative<std::int32_t>(args[5]) && variant->get<std::int32_t>(5) == 1, "login mode unchanged (type and value)");
                check(variant->get<std::string>(6) == "E2EPlayer", "username unchanged");
            }
        }
    }

    // 11. Client reconnects immediately (worst case: the old connection is not
    //     closed yet anywhere); the proxy must go to the sub-server.
    enet_peer_disconnect(client.peer, 0);
    enet_host_flush(client.host);
    client.connected = false;
    client.peer = enet_host_connect(client.host, &proxy_address, 2, 0);
    check(
        pump_until([&] { return client.connected && sub_server.connected; }),
        fmt::format("after reconnect the proxy connects to the sub-server {}:{}", SUB_SERVER_ADDRESS, SUB_SERVER_PORT)
    );
    check(game_server.connects == 1, "game server was not reconnected");
    check(pump_until([&] { return !game_server.connected; }), "proxy closed its connection to the old game server");

    send(sub_server, hello);
    {
        const auto got{ receive(client) };
        check(got && same_payload(hello, *got), "sub-server hello forwarded to client");
    }

    const Bytes sub_login{ text_packet(packet::NET_MESSAGE_GENERIC_TEXT, "protocol|225\nuser|555\ntoken|777001\nUUIDToken|e2e-uuid-token\n") };
    send(client, sub_login);
    {
        const auto got{ receive(sub_server) };
        check(got && same_payload(sub_login, *got), "sub-server login forwarded");
    }

    // 12. Large packet through the proxy (e.g. big world) in both directions.
    {
        Bytes big(200000);
        std::mt19937 rng{ 7 };
        for (auto& b : big) {
            b = static_cast<std::byte>(rng());
        }
        const Bytes big_packet{ map_data_packet(big) };
        send(sub_server, big_packet);
        const auto got{ receive(client, 10000ms) };
        check(got && same_payload(big_packet, *got), "200 KB packet forwarded server -> client");
    }

    // 13. Quit: proxy closes both sides.
    send(client, text_packet(packet::NET_MESSAGE_GAME_MESSAGE, "action|quit"));
    check(pump_until([&] { return !client.connected; }, 5000ms), "client disconnected after action|quit");
    check(pump_until([&] { return !sub_server.connected; }, 10000ms), "proxy disconnected from sub-server after quit");

    // 14. A new session works after quitting (proxy state was reset).
    client.peer = enet_host_connect(client.host, &proxy_address, 2, 0);
    check(pump_until([&] { return client.connected; }), "client can connect to the proxy again after quit");

    enet_host_destroy(client.host);
    enet_host_destroy(game_server.host);
    enet_host_destroy(sub_server.host);
    enet_deinitialize();

    fmt::print("\n{} failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
