// End-to-end harness: a fake Growtopia client and two fake game servers that
// talk to a running GTProxy over ENet, using the same ENet protocol flags as the
// game. Driven by tests/e2e/run_e2e.py, which also fakes server_data.php.
//
// Usage: GTProxy_e2e <items.dat path> [mismatch]
//   mismatch: the logon packet carries a different items.dat hash, so the proxy
//             must ask the client to download items.dat (hash 0).
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <enet/enet.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <glm/glm.hpp>
#include <httplib.h>

#include "item/item_database.hpp"
#include "map/coords.hpp"
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

// Session settings string of the official v5.61 logon packet (argument [5]).
constexpr const char* E2E_LOGON_SETTINGS{
    "proto=225|choosemusic=audio/mp3/about_theme.mp3|active_holiday=0|wing_week_day=0|"
    "enableInventoryTab=1|bigBackpack=1|seed_diary_hash=3856714732"
};

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

Bytes build_world(const std::uint16_t lock_item_id, const std::string& name = "E2ETESTWORLD")
{
    utils::ByteStream<> bs{};
    bs.write<std::uint16_t>(0x18);              // world version
    bs.write<std::uint32_t>(0);                 // unknown
    bs.write(name);

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

// Text of a NET_MESSAGE_GENERIC_TEXT / GAME_MESSAGE packet (without the 0).
std::optional<std::string> parse_text(const Bytes& data)
{
    if (data.size() < 4) {
        return std::nullopt;
    }

    std::uint32_t type{};
    std::memcpy(&type, data.data(), sizeof(type));
    if (type != packet::NET_MESSAGE_GENERIC_TEXT && type != packet::NET_MESSAGE_GAME_MESSAGE) {
        return std::nullopt;
    }

    std::string text{ reinterpret_cast<const char*>(data.data() + 4), data.size() - 4 };
    while (!text.empty() && text.back() == '\0') {
        text.pop_back();
    }
    return text;
}

Bytes map_data_packet(const Bytes& world)
{
    packet::GameUpdatePacket header{};
    header.type = packet::PACKET_SEND_MAP_DATA;
    header.net_id = static_cast<std::uint32_t>(-1);
    return game_packet(header, world);
}

std::string spawn_text(const int net_id, const std::string& name, const bool local)
{
    return fmt::format(
        "spawn|avatar\nnetID|{0}\nuserID|{1}\neid|e2e\nip|-\ncolrect|0|0|20|30\nposXY|64|96\nname|{2}\n"
        "titleIcon|{{\"PlayerWorldID\":{0},\"TitleTexture\":\"\"}}\ncountry|ru\ninvis|0\nmstate|0\nsmstate|0\n"
        "onlineID|\nfutureField|keep-me{3}",
        net_id, 1000 + net_id, name, local ? "\ntype|local" : ""
    );
}

Bytes call_with_net_id(const packet::PacketVariant& variant, const std::int32_t net_id)
{
    packet::GameUpdatePacket header{};
    header.type = packet::PACKET_CALL_FUNCTION;
    header.net_id = static_cast<std::uint32_t>(net_id);
    return game_packet(header, variant.serialize());
}

std::uint32_t net_id_of(const Bytes& data)
{
    packet::GameUpdatePacket header{};
    std::memcpy(&header, data.data() + sizeof(std::uint32_t), sizeof(header));
    return header.net_id;
}

// Next packet the endpoint receives that is a call of `function`.
std::optional<std::pair<packet::PacketVariant, Bytes>> receive_call(Endpoint& ep, const std::string& function, const std::chrono::milliseconds timeout = 3000ms)
{
    const auto deadline{ std::chrono::steady_clock::now() + timeout };
    while (std::chrono::steady_clock::now() < deadline) {
        const auto got{ receive(ep, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())) };
        if (!got) {
            break;
        }
        if (const auto variant{ parse_variant(*got) }; variant && variant->get<std::string>(0) == function) {
            return std::make_pair(*variant, *got);
        }
    }
    return std::nullopt;
}

std::optional<std::string> receive_text(Endpoint& ep, const std::chrono::milliseconds timeout = 3000ms)
{
    const auto got{ receive(ep, timeout) };
    return got ? parse_text(*got) : std::nullopt;
}

void input(Endpoint& client, const std::string& text)
{
    send(client, text_packet(packet::NET_MESSAGE_GENERIC_TEXT, "action|input\n|text|" + text));
}


Bytes inventory_packet(const std::vector<std::array<int, 3>>& items)
{
    utils::ByteStream<> bs{};
    bs.write<std::uint8_t>(1);
    bs.write<std::uint32_t>(96);
    bs.write<std::uint16_t>(static_cast<std::uint16_t>(items.size()));
    for (const auto& [id, count, flags] : items) {
        bs.write<std::uint16_t>(static_cast<std::uint16_t>(id));
        bs.write<std::uint8_t>(static_cast<std::uint8_t>(count));
        bs.write<std::uint8_t>(static_cast<std::uint8_t>(flags));
    }

    packet::GameUpdatePacket header{};
    header.type = packet::PACKET_SEND_INVENTORY_STATE;
    header.net_id = static_cast<std::uint32_t>(-1);
    return game_packet(header, bs.take_data());
}

Bytes drop_dialog(const int item_id, const int count)
{
    return variant_packet(packet::PacketVariant{
        std::string{ "OnDialogRequest" },
        fmt::format(
            "set_default_color|`o\nadd_label_with_icon|big|`wDrop Item``|left|{0}|\nadd_textbox|How many to drop?|left|\n"
            "add_text_input|count||{1}|5|\nembed_data|itemID|{0}\nend_dialog|drop_item|Cancel|OK|\n",
            item_id, count
        )
    });
}

Bytes removed_from_inventory(const int item_id, const int count)
{
    packet::GameUpdatePacket header{};
    header.type = packet::PACKET_MODIFY_ITEM_INVENTORY;
    header.net_id = static_cast<std::uint32_t>(-1);
    header.item_id = item_id;
    header.pad[1] = static_cast<std::uint8_t>(count);
    return game_packet(header, {});
}

// Plays the server for one drop: expects the request, opens the dialog,
// expects the answer, confirms. Returns the answered amount.
std::optional<int> serve_drop(Endpoint& client, Endpoint& game_server, const int item_id, const int owned)
{
    const auto request{ receive_text(game_server) };
    if (request != fmt::format("action|drop\n|itemID|{}\n", item_id)) {
        fmt::print("    expected a drop of {}, got: {}\n", item_id, request.value_or("nothing"));
        return std::nullopt;
    }

    send(game_server, drop_dialog(item_id, owned));
    const auto answer{ receive_text(game_server) };
    const std::string prefix{ fmt::format("action|dialog_return\ndialog_name|drop_item\nitemID|{}|\ncount|", item_id) };
    if (!answer || !answer->starts_with(prefix)) {
        fmt::print("    expected the drop answer for {}, got: {}\n", item_id, answer.value_or("nothing"));
        return std::nullopt;
    }

    const int amount{ std::stoi(answer->substr(prefix.size())) };
    send(game_server, removed_from_inventory(item_id, amount));
    return amount;
}

void run_item_checks(Endpoint& client, Endpoint& game_server, const std::uint16_t lock_item_id)
{
    // Drops need a world.
    send(game_server, map_data_packet(build_world(lock_item_id, "ITEMWORLD")));
    pump_for(300ms);
    client.inbox.clear();
    game_server.inbox.clear();

    input(client, "/drop dirt");
    {
        const auto reply{ receive_call(client, "OnConsoleMessage") };
        pump_for(200ms);
        check(reply && game_server.inbox.empty(), "/drop before the inventory is known does not guess");
    }

    // Dirt 200, Rock 50, World Lock 5, Diamond Lock 2, a marked item (0x02),
    // a worn item (0x01), Fist, Wrench.
    const Bytes inventory{ inventory_packet({
        { 18, 1, 0 }, { 32, 1, 0 }, { 2, 200, 0 }, { 10, 50, 0 }, { 242, 5, 0 }, { 1796, 2, 0 }, { 4, 3, 0x02 }, { 48, 1, 0x01 }
    }) };
    send(game_server, inventory);
    {
        const auto got{ receive(client) };
        check(got && same_payload(inventory, *got), "inventory packet forwarded unchanged");
    }

    // /daw: all locks, Diamond Locks first (higher value), then World Locks.
    input(client, "/daw");
    {
        const auto dl{ serve_drop(client, game_server, 1796, 2) };
        check(dl == 2, "/daw drops all Diamond Locks (answers the server's dialog with 2)");
        const auto wl{ serve_drop(client, game_server, 242, 5) };
        check(wl == 5, "/daw then drops all World Locks (5)");
        const auto done{ receive_call(client, "OnConsoleMessage") };
        bool finished{ false };
        for (auto call{ done }; call; call = receive_call(client, "OnConsoleMessage", 1500ms)) {
            if (call->first.get<std::string>(1).find("Drop finished") != std::string::npos) {
                finished = true;
                break;
            }
        }
        check(finished, "/daw reports when it is done");
        check(receive_call(client, "OnDialogRequest", 300ms) == std::nullopt, "the drop dialogs answered by the proxy never pop up in the game");
    }

    // /dw 3 drops only 3 of 5 World Locks.
    send(game_server, inventory);
    std::ignore = receive(client);
    input(client, "/dw 3");
    check(serve_drop(client, game_server, 242, 5) == 3, "/dw 3 answers the dialog with 3");
    pump_for(300ms);
    client.inbox.clear();

    // /dd <WL>: 300 WL from 5 DL + 5 WL = 3 Diamond Locks; 250 cannot be paid.
    {
        const Bytes locks{ inventory_packet({ { 18, 1, 0 }, { 242, 5, 0 }, { 1796, 5, 0 } }) };
        send(game_server, locks);
        std::ignore = receive(client);
        input(client, "/dd 300");
        check(serve_drop(client, game_server, 1796, 5) == 3, "/dd 300 drops 3 Diamond Locks");
        pump_for(500ms);
        bool more{ false };
        for (const auto& packet : game_server.inbox) {
            more = more || parse_text(packet).value_or("").starts_with("action|drop");
        }
        check(!more, "/dd 300 drops nothing else");
        client.inbox.clear();
        game_server.inbox.clear();

        send(game_server, locks);
        std::ignore = receive(client);
        input(client, "/dd 250");
        const auto reply{ receive_call(client, "OnConsoleMessage") };
        pump_for(300ms);
        check(
            reply && reply->first.get<std::string>(1).find("Can't make exactly") != std::string::npos && game_server.inbox.empty(),
            "/dd 250 with 5 DL + 5 WL explains it cannot pay exactly and drops nothing"
        );
        client.inbox.clear();
    }

    // A single World Lock: the server may drop it without the "how many?"
    // dialog; /dd must still count it as dropped (not as refused).
    {
        const Bytes one_lock{ inventory_packet({ { 18, 1, 0 }, { 32, 1, 0 }, { 242, 1, 0 } }) };
        send(game_server, one_lock);
        std::ignore = receive(client);
        input(client, "/dd");
        const auto request{ receive_text(game_server) };
        check(request == "action|drop\n|itemID|242\n", "/dd drops a single World Lock");
        send(game_server, removed_from_inventory(242, 1));
        bool dropped{ false };
        for (auto call{ receive_call(client, "OnConsoleMessage") }; call; call = receive_call(client, "OnConsoleMessage", 1000ms)) {
            const auto text{ call->first.get<std::string>(1) };
            if (text.find("Drop finished") != std::string::npos) {
                dropped = text.find("`w1`` stack(s) dropped") != std::string::npos && text.find("refused") == std::string::npos;
                break;
            }
        }
        check(dropped, "a drop without the dialog counts as dropped");
        client.inbox.clear();
    }

    // /dropall now: everything except favourites/worn/fist/wrench; biggest first.
    send(game_server, inventory);
    std::ignore = receive(client);
    input(client, "/dropall");
    {
        const auto call{ receive_call(client, "OnDialogRequest") };
        const std::string confirm{ call ? call->first.get<std::string>(1) : "" };
        check(confirm.find("end_dialog|gtproxy_dropall|") != std::string::npos && confirm.find("add_button|dropall_go|") != std::string::npos, "/dropall asks first and lists what it drops");
        pump_for(200ms);
        check(game_server.inbox.empty(), "/dropall drops nothing before you confirm");
    }
    send(client, text_packet(packet::NET_MESSAGE_GENERIC_TEXT, "action|dialog_return\ndialog_name|gtproxy_dropall\nbuttonClicked|dropall_go\n"));
    {
        std::vector<int> dropped;
        for (const auto& [id, owned] : std::vector<std::pair<int, int>>{ { 2, 200 }, { 10, 50 }, { 242, 5 }, { 1796, 2 } }) {
            if (serve_drop(client, game_server, id, owned) == owned) {
                dropped.push_back(id);
            }
        }
        check(dropped == std::vector<int>{ 2, 10, 242, 1796 }, fmt::format("confirmed /dropall drops Dirt, Rock, WL, DL in order ({})", fmt::join(dropped, ",")));
        pump_for(800ms);
        bool extra{ false };
        while (const auto text{ game_server.inbox.empty() ? std::nullopt : parse_text(game_server.inbox.front()) }) {
            extra = extra || text->starts_with("action|drop");
            game_server.inbox.pop_front();
        }
        check(!extra, "favourite (flagged), worn, Fist and Wrench are kept");
    }
    client.inbox.clear();

    // Fast drop: your own drop dialog is answered with the whole stack.
    input(client, "/fastdrop");
    std::ignore = receive_call(client, "OnConsoleMessage");
    send(game_server, drop_dialog(2, 200));
    {
        const auto answer{ receive_text(game_server) };
        check(answer == "action|dialog_return\ndialog_name|drop_item\nitemID|2|\ncount|200\n", "fast drop answers the drop dialog with the whole stack");
        check(receive_call(client, "OnDialogRequest", 300ms) == std::nullopt, "fast drop: the dialog does not pop up");
    }
    input(client, "/fastdrop");
    std::ignore = receive_call(client, "OnConsoleMessage");
    {
        const Bytes dialog{ drop_dialog(2, 200) };
        send(game_server, dialog);
        const auto got{ receive(client) };
        check(got && same_payload(dialog, *got), "with fast drop off the drop dialog reaches the game unchanged");
    }

    client.inbox.clear();
    game_server.inbox.clear();
}

void run_feature_checks(Endpoint& client, Endpoint& game_server, const std::uint16_t lock_item_id)
{
    client.inbox.clear();
    game_server.inbox.clear();

    // The server spawns us and another player; both reach the game unchanged.
    const std::string local_spawn{ spawn_text(5, "`wE2EPlayer``", true) };
    const std::string other_spawn{ spawn_text(6, "`2Other``", false) };
    for (const auto& text : { local_spawn, other_spawn }) {
        const Bytes spawn{ variant_packet(packet::PacketVariant{ std::string{ "OnSpawn" }, text }) };
        send(game_server, spawn);
        const auto got{ receive(client) };
        check(got && same_payload(spawn, *got), "OnSpawn forwarded unchanged while no feature is on");
    }

    // /nick: the game gets OnNameChanged for our netID with the title JSON.
    input(client, "/nick Tester");
    {
        const auto call{ receive_call(client, "OnNameChanged") };
        check(call.has_value(), "/nick sends OnNameChanged to the game");
        if (call) {
            const auto& [variant, bytes] = *call;
            check(net_id_of(bytes) == 5, "OnNameChanged targets the local netID");
            check(variant.size() == 3 && variant.get<std::string>(1) == "`wTester``", "nick shown as `wTester``");
            check(
                variant.size() == 3 && variant.get<std::string>(2) == "{\"PlayerWorldID\":5,\"TitleTexture\":\"\"}",
                "OnNameChanged carries the title JSON from OnSpawn (game 5.x needs it)"
            );
        }
        const auto reply{ receive_call(client, "OnConsoleMessage") };
        check(reply.has_value(), "/nick confirms in the console");
        pump_for(200ms);
        check(game_server.inbox.empty(), "/nick is not sent to the game server");
    }

    // The server renames us (e.g. after a title change): the nick stays.
    {
        const packet::PacketVariant rename{ std::string{ "OnNameChanged" }, std::string{ "`wE2EPlayer``" }, std::string{ "{\"t\":2}" } };
        send(game_server, call_with_net_id(rename, 5));
        const auto call{ receive_call(client, "OnNameChanged") };
        check(
            call && call->first.size() == 3 && call->first.get<std::string>(1) == "`wTester``" && call->first.get<std::string>(2) == "{\"t\":2}",
            "server OnNameChanged for us keeps the nick and the server's title"
        );
    }

    // Spawning again (re-entering) shows the nick; every other field is kept.
    {
        send(game_server, variant_packet(packet::PacketVariant{ std::string{ "OnSpawn" }, local_spawn }));
        const auto call{ receive_call(client, "OnSpawn") };
        std::string expected{ local_spawn };
        expected.replace(expected.find("`wE2EPlayer``"), std::string{ "`wE2EPlayer``" }.size(), "`wTester``");
        check(call && call->first.get<std::string>(1) == expected, "local OnSpawn shows the nick, all other fields unchanged");
    }

    // /nick without a name: back to the real name.
    input(client, "/nick");
    {
        const auto call{ receive_call(client, "OnNameChanged") };
        check(call && call->first.get<std::string>(1) == "`wE2EPlayer``", "/nick alone restores the real name");
        std::ignore = receive_call(client, "OnConsoleMessage");
    }

    // /netid shows netIDs next to both names; again hides them.
    input(client, "/netid");
    {
        std::vector<std::string> names;
        for (int i = 0; i < 2; ++i) {
            if (const auto call{ receive_call(client, "OnNameChanged") }) {
                names.push_back(call->first.get<std::string>(1));
            }
        }
        std::ranges::sort(names);
        check(
            names == std::vector<std::string>{ "`2Other`` `4[6]``", "`wE2EPlayer`` `4[5]``" },
            fmt::format("/netid shows netIDs next to names ({})", fmt::join(names, ", "))
        );
        std::ignore = receive_call(client, "OnConsoleMessage");
    }
    input(client, "/NetId");
    {
        int restored{ 0 };
        for (int i = 0; i < 2; ++i) {
            if (const auto call{ receive_call(client, "OnNameChanged") }; call && call->first.get<std::string>(1).find('[') == std::string::npos) {
                ++restored;
            }
        }
        check(restored == 2, "/NetId (any case) hides netIDs again");
        std::ignore = receive_call(client, "OnConsoleMessage");
    }

    // /proxy opens the menu dialog; its buttons are handled by the proxy.
    input(client, "/proxy");
    {
        const auto call{ receive_call(client, "OnDialogRequest") };
        const std::string dialog{ call ? call->first.get<std::string>(1) : "" };
        check(dialog.find("end_dialog|gtproxy_menu|") != std::string::npos, "/proxy opens the GTProxy menu");
        check(dialog.find("World: `wE2ETESTWORLD``") != std::string::npos, "menu shows the current world");
        check(
            dialog.find("add_button|page_worlds|") != std::string::npos && dialog.find("add_button|page_items|") != std::string::npos
                && dialog.find("add_button|page_looks|") != std::string::npos && dialog.find("add_button|page_info|") != std::string::npos,
            "menu has the Worlds / Items / Looks / Info pages"
        );
    }
    const auto open_page = [&](const std::string& button) {
        send(client, text_packet(packet::NET_MESSAGE_GENERIC_TEXT, "action|dialog_return\ndialog_name|gtproxy_menu\nbuttonClicked|" + button + "\n"));
        const auto call{ receive_call(client, "OnDialogRequest") };
        return call ? call->first.get<std::string>(1) : std::string{};
    };
    {
        const std::string worlds{ open_page("page_worlds") };
        check(worlds.find("end_dialog|gtproxy_worlds|") != std::string::npos && worlds.find("add_text_input|world|") != std::string::npos && worlds.find("add_button|warp|") != std::string::npos, "Worlds page has the warp input and button");
        const std::string looks{ open_page("page_looks") };
        check(looks.find("add_button|rainbow|") != std::string::npos && looks.find("add_checkbox|show_netid|") != std::string::npos, "Looks page offers rainbow (Lua) and netIDs");
        const std::string items{ open_page("page_items") };
        check(items.find("add_checkbox|fast_drop|") != std::string::npos && items.find("add_button|dropall|") != std::string::npos, "Items page has drop tools");
        pump_for(200ms);
        check(game_server.inbox.empty(), "menu pages are not sent to the game server");
    }

    send(client, text_packet(
        packet::NET_MESSAGE_GENERIC_TEXT,
        "action|dialog_return\ndialog_name|gtproxy_menu\nworld|\nnick|\nshow_netid|0\nfind|\nbuttonClicked|world_info\n"
    ));
    {
        const auto reply{ receive_call(client, "OnConsoleMessage") };
        check(reply && reply->first.get<std::string>(1).find("E2ETESTWORLD") != std::string::npos, "menu 'World info' answers with the world name");
        pump_for(200ms);
        check(game_server.inbox.empty(), "menu buttons are not sent to the game server");
    }

    send(client, text_packet(
        packet::NET_MESSAGE_GENERIC_TEXT,
        "action|dialog_return\ndialog_name|gtproxy_menu\nworld|\nnick|\nshow_netid|0\nfind|dirt\nbuttonClicked|find_item\n"
    ));
    {
        const auto reply{ receive_call(client, "OnConsoleMessage") };
        check(reply && reply->first.get<std::string>(1).find("matching 'dirt'") != std::string::npos, "menu item search answers");
    }

    {
        const Bytes server_dialog{ text_packet(packet::NET_MESSAGE_GENERIC_TEXT, "action|dialog_return\ndialog_name|popup\nnetID|6\nbuttonClicked|trade\n") };
        send(client, server_dialog);
        const auto got{ receive(game_server) };
        check(got && same_payload(server_dialog, *got), "the game's own dialogs still go to the server");
    }

    // /warp from inside a world: exit first, join after the world menu.
    input(client, "/warp nextworld");
    {
        const auto quit{ receive_text(game_server) };
        check(quit == "action|quit_to_exit", "/warp exits the current world first");
        send(game_server, variant_packet(packet::PacketVariant{ std::string{ "OnRequestWorldSelectMenu" }, std::string{ "default|\nadd_filter|\n" } }));
        const auto join{ receive_text(game_server) };
        check(join == "action|join_request\nname|NEXTWORLD\ninvitedWorld|0", fmt::format("after the world menu /warp joins NEXTWORLD ({})", join.value_or("nothing")));
        check(receive_call(client, "OnRequestWorldSelectMenu").has_value(), "world menu still reaches the game");
    }

    // Enter NEXTWORLD, then /back returns to E2ETESTWORLD the same way.
    send(game_server, map_data_packet(build_world(lock_item_id, "NEXTWORLD")));
    pump_for(300ms);
    client.inbox.clear();
    input(client, "/back");
    {
        const auto quit{ receive_text(game_server) };
        check(quit == "action|quit_to_exit", "/back exits NEXTWORLD first");
        send(game_server, variant_packet(packet::PacketVariant{ std::string{ "OnRequestWorldSelectMenu" }, std::string{ "default|\n" } }));
        const auto join{ receive_text(game_server) };
        check(join == "action|join_request\nname|E2ETESTWORLD\ninvitedWorld|0", fmt::format("/back joins the previous world ({})", join.value_or("nothing")));
    }

    // From the world menu /warp joins at once; a door id is passed on.
    input(client, "/warp start shop_1");
    {
        const auto join{ receive_text(game_server) };
        check(join == "action|join_request\nname|START|SHOP_1\ninvitedWorld|0", fmt::format("/warp with a door from the menu ({})", join.value_or("nothing")));
    }

    input(client, "/warp bad-name");
    {
        const auto reply{ receive_call(client, "OnConsoleMessage") };
        pump_for(200ms);
        check(reply.has_value() && game_server.inbox.empty(), "/warp rejects invalid world names without sending anything");
    }

    client.inbox.clear();
    game_server.inbox.clear();
}
}

namespace {
bool g_modules{ true };

// Without modules/ the proxy is an empty core: it forwards everything (the
// features' commands are plain chat for the server) and only keeps track of
// where you are; /proxy shows the menu frame alone.
void run_empty_core_checks(Endpoint& client, Endpoint& game_server, const std::uint16_t lock_item_id)
{
    std::ignore = lock_item_id;
    client.inbox.clear();
    game_server.inbox.clear();

    for (const auto& text : { spawn_text(5, "`wE2EPlayer``", true), spawn_text(6, "`2Other``", false) }) {
        const Bytes spawn{ variant_packet(packet::PacketVariant{ std::string{ "OnSpawn" }, text }) };
        send(game_server, spawn);
        const auto got{ receive(client) };
        check(got && same_payload(spawn, *got), "no modules: OnSpawn forwarded unchanged");
    }

    for (const std::string command : { "/nick Tester", "/netid", "/warp start", "/dropall now", "/daw", "/goto 3 6", "/pos", "/find dirt" }) {
        const Bytes chat{ text_packet(packet::NET_MESSAGE_GENERIC_TEXT, "action|input\n|text|" + command) };
        send(client, chat);
        const auto got{ receive(game_server) };
        check(got && same_payload(chat, *got), fmt::format("no modules: {} is not a proxy command (goes to the server)", command));
    }
    pump_for(200ms);
    check(client.inbox.empty(), "no modules: the proxy answers none of them");

    {
        const Bytes dialog{ drop_dialog(2, 200) };
        send(game_server, dialog);
        const auto got{ receive(client) };
        check(got && same_payload(dialog, *got), "no modules: the server's drop dialog reaches the game unchanged");
    }

    input(client, "/proxy");
    {
        const auto call{ receive_call(client, "OnDialogRequest") };
        const std::string dialog{ call ? call->first.get<std::string>(1) : "" };
        check(
            dialog.find("end_dialog|gtproxy_menu|") != std::string::npos && dialog.find("World: `wE2ETESTWORLD``") != std::string::npos
                && dialog.find("No modules") != std::string::npos && dialog.find("add_button|map|") != std::string::npos
                && dialog.find("page_items") == std::string::npos,
            "no modules: /proxy shows the menu frame (where you are, map, commands) and no module pages"
        );
    }
    send(client, text_packet(packet::NET_MESSAGE_GENERIC_TEXT, "action|dialog_return\ndialog_name|gtproxy_menu\nbuttonClicked|page_items\n"));
    pump_for(300ms);
    check(game_server.inbox.empty() && client.inbox.empty(), "no modules: a module page's button is ignored and never sent to the server");

    {
        const Bytes server_dialog{ text_packet(packet::NET_MESSAGE_GENERIC_TEXT, "action|dialog_return\ndialog_name|popup\nnetID|6\nbuttonClicked|trade\n") };
        send(client, server_dialog);
        const auto got{ receive(game_server) };
        check(got && same_payload(server_dialog, *got), "no modules: the game's own dialogs still go to the server");
    }

    client.inbox.clear();
    game_server.inbox.clear();
}

// --- Map window: click -> tile -> walk ---------------------------------------
constexpr int MAP_PORT{ 17171 };
constexpr int MAP_W{ 12 };

// Set by run_e2e.py: E2E_NO_MODULES=1 runs the proxy without modules/
// (g_modules, above); E2E_APP_DIR is the folder of the proxy's executable
// (modules/, config/).
std::filesystem::path g_app_dir;
constexpr int MAP_H{ 8 };

// 12x8 world: dirt floor (bottom row), a rock wall at x = 6 (rows 3-6) and
// lava at 9, 6.
std::uint16_t map_fg(const int x, const int y)
{
    if (y == MAP_H - 1) {
        return 2;
    }
    if (x == 6 && y >= 3) {
        return 10;
    }
    if (x == 9 && y == 6) {
        return 4;
    }
    return 0;
}

bool map_free(const map::TilePos tile)
{
    return map::in_bounds(tile, { MAP_W, MAP_H }) && map_fg(tile.x, tile.y) == 0;
}

Bytes build_map_world()
{
    utils::ByteStream<> bs{};
    bs.write<std::uint16_t>(0x18);
    bs.write<std::uint32_t>(0);
    bs.write(std::string{ "MAPTEST" });
    bs.write<std::int32_t>(MAP_W);
    bs.write<std::int32_t>(MAP_H);
    bs.write<std::uint32_t>(MAP_W * MAP_H);
    for (int i = 0; i < 5; ++i) {
        bs.write<std::uint8_t>(0);
    }
    for (int y = 0; y < MAP_H; ++y) {
        for (int x = 0; x < MAP_W; ++x) {
            bs.write<std::uint16_t>(map_fg(x, y));
            bs.write<std::uint16_t>(14);
            bs.write<std::uint16_t>(0);
            bs.write<std::uint16_t>(0);
        }
    }
    for (int i = 0; i < 12; ++i) {
        bs.write<std::uint8_t>(0);
    }
    bs.write<std::uint32_t>(0);
    bs.write<std::uint32_t>(0);
    for (int i = 0; i < 16; ++i) {
        bs.write<std::uint8_t>(0xAA);
    }
    return bs.take_data();
}

std::optional<packet::GameUpdatePacket> game_header(const Bytes& data)
{
    std::uint32_t type{};
    if (data.size() < sizeof(type) + sizeof(packet::GameUpdatePacket)) {
        return std::nullopt;
    }
    std::memcpy(&type, data.data(), sizeof(type));
    if (type != packet::NET_MESSAGE_GAME_PACKET) {
        return std::nullopt;
    }
    packet::GameUpdatePacket header{};
    std::memcpy(&header, data.data() + sizeof(type), sizeof(header));
    return header;
}

Bytes state_packet(const glm::vec2 position, const std::uint32_t flags)
{
    packet::GameUpdatePacket header{};
    header.type = packet::PACKET_STATE;
    header.net_id = 5;
    header.flags.value = static_cast<packet::PacketFlag>(flags);
    header.pos_x = position.x;
    header.pos_y = position.y;
    header.int_x = -1;
    header.int_y = -1;
    return game_packet(header, {});
}

Bytes tile_change(const int x, const int y, const int item)
{
    packet::GameUpdatePacket header{};
    header.type = packet::PACKET_TILE_CHANGE_REQUEST;
    header.net_id = 5;
    header.int_x = x;
    header.int_y = y;
    header.int_data = item;
    return game_packet(header, {});
}

// Numbers of a JSON array field, e.g. "fg":[0,2,...].
std::vector<long long> json_numbers(const std::string& json, const std::string& key)
{
    std::vector<long long> values;
    const auto start{ json.find("\"" + key + "\":[") };
    if (start == std::string::npos) {
        return values;
    }
    std::size_t i{ start + key.size() + 4 };
    while (i < json.size() && json[i] != ']') {
        std::size_t used{};
        values.push_back(std::stoll(json.substr(i), &used));
        i += used;
        if (i < json.size() && json[i] == ',') {
            ++i;
        }
    }
    return values;
}

struct WalkLog {
    std::vector<packet::GameUpdatePacket> states;   // what the server got
    std::vector<Bytes> other_to_server;
    std::vector<std::pair<std::uint32_t, glm::vec2>> set_pos; // what the game got
    std::vector<std::string> console;

    [[nodiscard]] bool said(const std::string_view text) const
    {
        return std::ranges::any_of(console, [&](const std::string& line) { return line.find(text) != std::string::npos; });
    }
};

void collect(Endpoint& client, Endpoint& game_server, WalkLog& log)
{
    while (!game_server.inbox.empty()) {
        Bytes data{ std::move(game_server.inbox.front()) };
        game_server.inbox.pop_front();
        if (const auto header{ game_header(data) }; header && header->type == packet::PACKET_STATE) {
            log.states.push_back(*header);
        } else {
            log.other_to_server.push_back(std::move(data));
        }
    }
    while (!client.inbox.empty()) {
        Bytes data{ std::move(client.inbox.front()) };
        client.inbox.pop_front();
        const auto variant{ parse_variant(data) };
        if (!variant) {
            continue;
        }
        const auto function{ variant->get<std::string>(0) };
        if (function == "OnSetPos") {
            log.set_pos.emplace_back(net_id_of(data), variant->get<glm::vec2>(1));
        } else if (function == "OnConsoleMessage") {
            log.console.push_back(variant->get<std::string>(1));
        }
    }
}

bool follow_walk(Endpoint& client, Endpoint& game_server, WalkLog& log, const std::string_view until, const std::chrono::milliseconds timeout = 10000ms)
{
    return pump_until([&] {
        collect(client, game_server, log);
        return log.said(until);
    }, timeout);
}

// GETs a map window URL until the body passes `ok` (the state is published a
// few times a second once a window asks for it).
std::optional<std::string> wait_json(httplib::Client& http, const std::string& path, const std::function<bool(const std::string&)>& ok, const std::chrono::milliseconds timeout = 3000ms)
{
    const auto deadline{ std::chrono::steady_clock::now() + timeout };
    std::string last;
    while (std::chrono::steady_clock::now() < deadline) {
        if (const auto res{ http.Get(path) }; res && res->status == 200) {
            last = res->body;
            if (ok(last)) {
                return last;
            }
        }
        pump_for(50ms);
    }
    fmt::print("    last {}: {}\n", path, last.substr(0, 400));
    return std::nullopt;
}

std::optional<std::string> map_post(httplib::Client& http, const std::string& path)
{
    const httplib::Headers headers{ { "X-GTProxy-Map", "1" } };
    const auto res{ http.Post(path, headers, "", "text/plain") };
    if (!res || res->status != 200) {
        return std::nullopt;
    }
    return res->body;
}

// A click in the window at the centre of a tile (view: no scroll, zoom 1).
std::optional<std::string> click_tile(httplib::Client& http, const int x, const int y, const int button)
{
    return map_post(http, fmt::format(
        "/api/click?x={}&y={}&button={}&ox=0&oy=0&zoom=1&w=800&h=600",
        x * 32 + 16, y * 32 + 16, button
    ));
}

bool contains(const std::optional<std::string>& text, const std::string_view part)
{
    return text && text->find(part) != std::string::npos;
}

void run_map_checks(Endpoint& client, Endpoint& game_server)
{
    httplib::Client http{ "127.0.0.1", MAP_PORT };
    http.set_connection_timeout(2);
    http.set_read_timeout(5);

    {
        const auto res{ http.Get("/") };
        check(res && res->status == 200 && res->body.find("<canvas id=\"map\">") != std::string::npos, "map window page is served on 127.0.0.1:17171");
    }

    // Enter MAPTEST and spawn at tile 1, 6 (standing on the dirt floor).
    client.inbox.clear();
    game_server.inbox.clear();
    send(client, text_packet(packet::NET_MESSAGE_GAME_MESSAGE, "action|join_request\nname|MAPTEST\ninvitedWorld|0\n"));
    check(receive(game_server).has_value(), "map: join request forwarded");
    send(game_server, map_data_packet(build_map_world()));
    check(receive(client).has_value(), "map: world data forwarded");
    const std::string spawn{ fmt::format(
        "spawn|avatar\nnetID|5\nuserID|1005\ncolrect|0|0|20|30\nposXY|32|192\nname|`wE2EPlayer``\ncountry|ru\ninvis|0\nmstate|0\nsmstate|0\nonlineID|\ntype|local"
    ) };
    send(game_server, variant_packet(packet::PacketVariant{ std::string{ "OnSpawn" }, spawn }));
    check(receive_call(client, "OnSpawn").has_value(), "map: local OnSpawn forwarded");

    // The game's own movement: forwarded, and the model for the proxy's packets.
    constexpr std::uint32_t GAME_FLAGS{ packet::PACKET_FLAG_UNK | packet::PACKET_FLAG_ON_SOLID };
    send(client, state_packet({ 32.0f, 192.0f }, GAME_FLAGS));
    {
        const auto got{ receive(game_server) };
        const auto header{ got ? game_header(*got) : std::nullopt };
        check(header && header->type == packet::PACKET_STATE && header->pos_x == 32.0f, "map: the game's state packets are forwarded while not walking");
    }

    {
        const auto state{ wait_json(http, "/api/state", [](const std::string& body) {
            return body.find("\"world\":\"MAPTEST\"") != std::string::npos && body.find("\"tx\":1,\"ty\":6,\"me\":true") != std::string::npos;
        }) };
        check(state.has_value(), "map: /api/state shows the world and you at tile 1, 6");

        const auto tiles{ wait_json(http, "/api/tiles", [](const std::string& body) {
            return body.find("\"width\":12,\"height\":8") != std::string::npos;
        }) };
        check(tiles.has_value(), "map: /api/tiles has the 12x8 world");
        if (tiles) {
            const auto fg{ json_numbers(*tiles, "fg") };
            const auto cell{ json_numbers(*tiles, "cell") };
            check(fg.size() == 96 && fg[7 * 12] == 2 && fg[4 * 12 + 6] == 10, "map: tile ids (dirt floor, rock wall)");
            check(cell.size() == 96 && cell[7 * 12] == 1 && cell[6 * 12 + 9] == 3 && cell[2 * 12 + 2] == 0, "map: walkability (solid, dangerous, free) from items.dat");
            check(tiles->find("\"2\":{\"name\":\"Dirt\",\"color\":\"#603913\"}") != std::string::npos, "map: block colour from its seed (Dirt brown)");
        }
    }

    // Only pages from this computer may use the map.
    {
        const auto no_header{ http.Post("/api/click?x=1&y=1&button=0&ox=0&oy=0&zoom=1&w=800&h=600", "", "text/plain") };
        check(no_header && no_header->status == 403, "map: a click without the window's header is refused");
        const auto foreign{ http.Get("/api/state", httplib::Headers{ { "Host", "evil.example:17171" } }) };
        check(foreign && foreign->status == 403, "map: requests for another host name are refused (DNS rebinding)");
    }

    // Clicks that do nothing.
    check(contains(map_post(http, "/api/click?x=500&y=10&button=0&ox=0&oy=0&zoom=1&w=800&h=600"), "outside the world"), "map: a click beside the world is reported, nothing sent");
    {
        const auto bad{ http.Post("/api/click?x=a&y=1", httplib::Headers{ { "X-GTProxy-Map", "1" } }, "", "text/plain") };
        check(bad && bad->status == 400, "map: a malformed click is rejected");
    }

    // Right click: the reachability check, with what is on the tile.
    {
        const auto info{ click_tile(http, 0, 7, 2) };
        if (g_modules) {
            check(
                contains(info, "\"action\":\"card\"") && contains(info, "\"reach\":\"blocked\"")
                    && contains(info, "Dirt (2) on Cave Background (14) - solid"),
                fmt::format("map: right click on a block: rejected, with the tile's contents ({})", info.value_or("-"))
            );
        }
        else {
            check(contains(info, "no module handles it"), fmt::format("map: without modules a right click says nobody answers it ({})", info.value_or("-")));
        }
    }

    if (!g_modules) {
        // Nothing walks without modules/walk: the click is not answered,
        // /goto and /clickwalk are chat for the server, punches pass.
        const auto answer{ click_tile(http, 10, 6, 0) };
        check(contains(answer, "Left click does nothing: no module handles it"), fmt::format("no modules: a left click is not answered ({})", answer.value_or("-")));
        pump_for(300ms);
        WalkLog log{};
        collect(client, game_server, log);
        check(log.states.empty() && log.set_pos.empty(), "no modules: nothing walks");

        for (const std::string command : { "/goto 3 6", "/clickwalk" }) {
            const Bytes chat{ text_packet(packet::NET_MESSAGE_GENERIC_TEXT, "action|input\n|text|" + command) };
            send(client, chat);
            const auto got{ receive(game_server) };
            check(got && same_payload(chat, *got), fmt::format("no modules: {} goes to the server", command));
        }
        send(client, tile_change(5, 6, 18));
        const auto got{ receive(game_server) };
        const auto header{ got ? game_header(*got) : std::nullopt };
        check(header && header->type == packet::PACKET_TILE_CHANGE_REQUEST && header->int_x == 5, "no modules: punches reach the server");
    }
    else {
    // Left click on 10, 6: walk over the rock wall, around the lava.
    {
        WalkLog log{};
        const auto answer{ click_tile(http, 10, 6, 0) };
        check(contains(answer, "\"ok\":true") && contains(answer, "\"tile\":{\"x\":10,\"y\":6}") && contains(answer, "Walking to 10, 6"), fmt::format("map: left click walks to the tile ({})", answer.value_or("-")));

        // The game keeps sending its own position: held back while walking.
        send(client, state_packet({ 999.0f, 999.0f }, GAME_FLAGS));
        const bool arrived{ follow_walk(client, game_server, log, "Arrived at 10, 6") };
        check(arrived, "map: the walk ends with 'Arrived at 10, 6' in the game console");
        // The last position to the server may still be on its way.
        pump_for(300ms);
        collect(client, game_server, log);

        check(log.states.size() >= 5, fmt::format("map: {} state packets sent to the server while walking", log.states.size()));
        bool fields_ok{ true };
        bool tiles_ok{ true };
        bool model_kept{ true };
        float longest{ 0.0f };
        glm::vec2 previous{ 32.0f, 192.0f };
        for (const auto& header : log.states) {
            const glm::vec2 position{ header.pos_x, header.pos_y };
            fields_ok = fields_ok && header.net_id == 5 && header.int_x == -1 && header.int_y == -1 && position.x != 999.0f;
            model_kept = model_kept && (static_cast<std::uint32_t>(header.flags.value) & packet::PACKET_FLAG_UNK) != 0;
            tiles_ok = tiles_ok && map_free(map::player_tile(position));
            longest = std::max(longest, glm::length(position - previous));
            previous = position;
        }
        check(fields_ok, "map: walk packets carry your netID, no punch target, and the game's held-back position never gets through");
        check(model_kept, "map: walk packets keep the game's own flags");
        check(tiles_ok, "map: every position is in a free tile (over the wall, not through it or the lava)");
        check(longest > 0.0f && longest <= 40.0f, fmt::format("map: positions at most {:.1f} px apart (walking speed, not teleporting)", longest));
        check(!log.states.empty() && log.states.back().pos_x == 320.0f && log.states.back().pos_y == 192.0f, "map: the last position is tile 10, 6 (320, 192)");
        check(
            log.set_pos.size() == log.states.size() && !log.set_pos.empty()
                && log.set_pos.back().first == 5 && log.set_pos.back().second == glm::vec2{ 320.0f, 192.0f },
            fmt::format("map: the game is moved along with OnSetPos ({} of them)", log.set_pos.size())
        );
        check(log.other_to_server.empty(), "map: nothing else reaches the server");

        const auto state{ wait_json(http, "/api/state", [](const std::string& body) {
            return body.find("\"walking\":false") != std::string::npos && body.find("\"tx\":10,\"ty\":6,\"me\":true") != std::string::npos;
        }) };
        check(contains(state, "Arrived at 10, 6"), "map: the window shows you at 10, 6 and the result");

        // The game's packets go through again.
        send(client, state_packet({ 320.0f, 192.0f }, GAME_FLAGS));
        const auto got{ receive(game_server) };
        const auto header{ got ? game_header(*got) : std::nullopt };
        check(header && header->type == packet::PACKET_STATE && header->pos_x == 320.0f, "map: after the walk the game's state packets are forwarded again");
    }

    // Click on the rock wall: walk next to it; Stop ends the walk.
    {
        WalkLog log{};
        const auto answer{ click_tile(http, 6, 4, 0) };
        check(contains(answer, "Walking next to 6, 4"), fmt::format("map: a click on a block walks next to it ({})", answer.value_or("-")));
        const auto stopped{ map_post(http, "/api/stop") };
        check(contains(stopped, "Stopped at"), fmt::format("map: Stop ends the walk ({})", stopped.value_or("-")));
        pump_for(300ms);
        collect(client, game_server, log);
        const auto sent{ log.states.size() };
        pump_for(300ms);
        collect(client, game_server, log);
        check(log.states.size() == sent, "map: no more positions after Stop");
    }

    // /goto in the chat; the server moving you (door, respawn) ends the walk.
    {
        WalkLog log{};
        input(client, "/goto 3 6");
        check(follow_walk(client, game_server, log, "[map] Walking to 3, 6", 3000ms), "map: /goto 3 6 walks to the tile");
        check(log.other_to_server.empty(), "map: /goto is not sent to the server");

        packet::GameUpdatePacket header{};
        header.type = packet::PACKET_CALL_FUNCTION;
        header.net_id = 5;
        send(game_server, game_packet(header, packet::PacketVariant{ std::string{ "OnSetPos" }, glm::vec2{ 64.0f, 192.0f } }.serialize()));
        check(follow_walk(client, game_server, log, "the server put you at 2, 6", 3000ms), "map: OnSetPos from the server stops the walk");
        check(std::ranges::any_of(log.set_pos, [](const auto& p) { return p.second == glm::vec2{ 64.0f, 192.0f }; }), "map: the server's OnSetPos still reaches the game");

        const auto state{ wait_json(http, "/api/state", [](const std::string& body) {
            return body.find("\"tx\":2,\"ty\":6,\"me\":true") != std::string::npos;
        }) };
        check(state.has_value(), "map: OnSetPos moves you on the map too");

        WalkLog usage{};
        input(client, "/goto x");
        check(follow_walk(client, game_server, usage, "Usage", 3000ms), "map: /goto with bad arguments shows the usage");
    }

    // /clickwalk: a punch in the game becomes a walk; off again, punches pass.
    {
        WalkLog log{};
        input(client, "/clickwalk");
        check(follow_walk(client, game_server, log, "Click-walk", 3000ms) && log.said("on"), "map: /clickwalk turns click-walk on");
        send(client, tile_change(4, 6, 18));
        check(follow_walk(client, game_server, log, "Arrived at 4, 6"), "map: with click-walk a click in the game walks to that tile");
        check(log.other_to_server.empty(), "map: the click (punch) is not sent to the server");

        input(client, "/clickwalk");
        check(follow_walk(client, game_server, log, "Click-walk `4off", 3000ms), "map: /clickwalk again turns it off");
        // Let the last position of the walk arrive first.
        pump_for(300ms);
        collect(client, game_server, log);
        log.other_to_server.clear();

        send(client, tile_change(5, 6, 18));
        const bool punched{ pump_until([&] {
            collect(client, game_server, log);
            return std::ranges::any_of(log.other_to_server, [](const Bytes& data) {
                const auto header{ game_header(data) };
                return header && header->type == packet::PACKET_TILE_CHANGE_REQUEST && header->int_x == 5;
            });
        }, 3000ms) };
        check(punched, "map: without click-walk punches reach the server");
    }
    }

    // The map follows what the server changes.
    {
        send(game_server, tile_change(0, 7, 18));      // dirt punched away
        utils::ByteStream<> tile{};
        tile.write<std::uint16_t>(10);                  // rock
        tile.write<std::uint16_t>(14);
        tile.write<std::uint16_t>(0);
        tile.write<std::uint16_t>(0);
        packet::GameUpdatePacket header{};
        header.type = packet::PACKET_SEND_TILE_UPDATE_DATA;
        header.int_x = 8;
        header.int_y = 5;
        send(game_server, game_packet(header, tile.take_data()));

        const auto tiles{ wait_json(http, "/api/tiles", [](const std::string& body) {
            const auto fg{ json_numbers(body, "fg") };
            return fg.size() == 96 && fg[7 * 12] == 0 && fg[5 * 12 + 8] == 10;
        }) };
        check(tiles.has_value(), "map: tiles broken (TileChangeRequest) and re-sent (SendTileUpdateData) by the server show up");
        if (tiles) {
            const auto cell{ json_numbers(*tiles, "cell") };
            check(cell.size() == 96 && cell[7 * 12] == 0 && cell[5 * 12 + 8] == 1, "map: and count for walking");
        }
    }

    client.inbox.clear();
    game_server.inbox.clear();
}

// --- Right click: can the target be reached from where you stand ------------
constexpr int BOX_W{ 12 };
constexpr int BOX_H{ 9 };

// 12x9 world, you stand at 5, 4 inside a rock box (x 3-7, y 2-6). Also: a
// closed room around 10, 3, a pocket at 10, 7 open only diagonally, a lone
// block at 1, 2 and a dirt floor.
std::uint16_t box_fg(const int x, const int y)
{
    if (y == BOX_H - 1) {
        return 2;
    }
    if ((x == 3 || x == 7) && y >= 2 && y <= 6) {
        return 10;
    }
    if ((y == 2 || y == 6) && x >= 3 && x <= 7) {
        return 10;
    }
    if (((x == 9 || x == 11) && y >= 2 && y <= 4) || ((y == 2 || y == 4) && x == 10)) {
        return 10;
    }
    if ((x == 10 && y == 6) || (x == 9 && y == 7) || (x == 11 && y == 7)) {
        return 10;
    }
    if (x == 1 && y == 2) {
        return 10;
    }
    return 0;
}

Bytes build_box_world()
{
    utils::ByteStream<> bs{};
    bs.write<std::uint16_t>(0x18);
    bs.write<std::uint32_t>(0);
    bs.write(std::string{ "BOXTEST" });
    bs.write<std::int32_t>(BOX_W);
    bs.write<std::int32_t>(BOX_H);
    bs.write<std::uint32_t>(BOX_W * BOX_H);
    for (int i = 0; i < 5; ++i) {
        bs.write<std::uint8_t>(0);
    }
    for (int y = 0; y < BOX_H; ++y) {
        for (int x = 0; x < BOX_W; ++x) {
            bs.write<std::uint16_t>(box_fg(x, y));
            bs.write<std::uint16_t>(14);
            bs.write<std::uint16_t>(0);
            bs.write<std::uint16_t>(0);
        }
    }
    for (int i = 0; i < 12; ++i) {
        bs.write<std::uint8_t>(0);
    }
    bs.write<std::uint32_t>(0);
    bs.write<std::uint32_t>(0);
    return bs.take_data();
}

// Right click in the window on a tile (view: no scroll, zoom 1, or scrolled
// left by 'ox' world pixels).
std::optional<std::string> right_click(httplib::Client& http, const int x, const int y, const int ox = 0)
{
    return map_post(http, fmt::format(
        "/api/click?x={}&y={}&button=2&ox={}&oy=0&zoom=1&w=800&h=600",
        x * 32 + 16 - ox, y * 32 + 16, ox
    ));
}

std::optional<std::string> game_reply(Endpoint& client, Endpoint& game_server, const std::string& command)
{
    WalkLog log{};
    input(client, command);
    if (!follow_walk(client, game_server, log, "[map]", 3000ms)) {
        return std::nullopt;
    }
    return log.console.back();
}

// Has the proxy's console run a command (run_e2e.py types it in).
void console(const std::string& command)
{
    fmt::print("@console {}\n", command);
    std::fflush(stdout);
}

std::string read_app_file(const std::string& relative)
{
    std::ifstream in{ g_app_dir / relative, std::ios::binary };
    return { std::istreambuf_iterator<char>{ in }, {} };
}

void write_app_file(const std::string& relative, const std::string& text)
{
    std::ofstream{ g_app_dir / relative, std::ios::binary | std::ios::trunc } << text;
}

// Right clicks until the answer passes `ok`: a console command takes effect
// once the proxy has read it.
std::optional<std::string> right_click_until(
    httplib::Client& http,
    const int x,
    const int y,
    const std::function<bool(const std::string&)>& ok,
    const std::chrono::milliseconds timeout = 5000ms
)
{
    const auto deadline{ std::chrono::steady_clock::now() + timeout };
    std::optional<std::string> last;
    while (std::chrono::steady_clock::now() < deadline) {
        last = right_click(http, x, y);
        if (last && ok(*last)) {
            return last;
        }
        pump_for(100ms);
    }
    fmt::print("    last answer for {}, {}: {}\n", x, y, last.value_or("-"));
    return last;
}

// Console line of the game that contains `part`, after typing `command`.
std::optional<std::string> game_reply_with(Endpoint& client, Endpoint& game_server, const std::string& command, const std::string& part)
{
    WalkLog log{};
    input(client, command);
    if (!follow_walk(client, game_server, log, part, 3000ms)) {
        return std::nullopt;
    }
    return log.console.back();
}

bool has(const std::string& text, const std::string_view part)
{
    return text.find(part) != std::string::npos;
}

// Profiles, settings files and switching the module off and on, all while
// the proxy runs (in BOXTEST with the box open, you at 5, 4).
void run_profile_checks(Endpoint& client, Endpoint& game_server, httplib::Client& http)
{
    const std::string modules_json{ read_app_file("config/modules.json") };
    const std::string module_settings{ read_app_file("config/modules/reachability.json") };
    const auto marker_on = [](const std::string& body) { return has(body, "\"marker\":true"); };
    const auto marker_off = [](const std::string& body) { return has(body, "\"marker\":false"); };
    const auto steps_on = [](const std::string& body) { return has(body, "5 шагов"); };
    const auto steps_off = [](const std::string& body) { return has(body, "Цель доступна") && !has(body, "шагов"); };

    auto answer{ right_click(http, 0, 4) };
    check(contains(answer, "5 шагов") && contains(answer, "\"marker\":true"), "profiles: default profile - step count and marker");

    console("cfg list");
    console("cfg exec test");
    answer = right_click_until(http, 0, 4, marker_off);
    check(answer && marker_off(*answer) && steps_off(*answer), fmt::format("profiles: cfg exec test applies without a restart - no marker, no step count ({})", answer.value_or("-")));
    {
        const auto chat{ game_reply(client, game_server, "/reach 0 4") };
        check(contains(chat, "target reachable") && !contains(chat, "steps"), fmt::format("profiles: /reach follows the profile ({})", chat.value_or("-")));
    }

    // Broken JSON, then a value of the wrong type: "test" stays in force.
    console("cfg exec broken");
    console("cfg exec wrongtype");
    pump_for(1000ms);
    answer = right_click(http, 0, 4);
    check(answer && marker_off(*answer) && steps_off(*answer), "profiles: after broken JSON and a wrong type the working settings stay");

    // defaults -> config/modules/reachability.json -> profile
    write_app_file("config/modules/reachability.json", R"({ "show_steps": false })");
    console("cfg exec default");
    answer = right_click_until(http, 0, 4, marker_on);
    check(answer && marker_on(*answer) && steps_off(*answer), "profiles: config/modules/reachability.json overrides the module's default");
    console("cfg exec steps");
    answer = right_click_until(http, 0, 4, steps_on);
    check(answer && steps_on(*answer), "profiles: the profile overrides config/modules/reachability.json");
    console("cfg exec default");
    answer = right_click_until(http, 0, 4, steps_off);
    check(answer && steps_off(*answer), "profiles: back to default - nothing of the previous profile is left");

    // A broken settings file on reload: kept; fixed: applied.
    write_app_file("config/modules/reachability.json", R"({ "show_steps": tru)");
    console("cfg reload");
    pump_for(1000ms);
    answer = right_click(http, 0, 4);
    check(answer && steps_off(*answer), "profiles: cfg reload with a broken settings file keeps the working settings");
    write_app_file("config/modules/reachability.json", module_settings);
    console("cfg reload");
    answer = right_click_until(http, 0, 4, steps_on);
    check(answer && steps_on(*answer), "profiles: the fixed file applies on cfg reload");

    // log_result off (the runner checks that 2, 7 is not in the log).
    console("cfg exec quiet");
    answer = right_click_until(http, 0, 0, marker_off);
    check(answer && marker_off(*answer), "profiles: quiet profile applied");
    answer = right_click(http, 2, 7);
    check(contains(answer, "Цель доступна"), "profiles: with log_result off the check still answers");
    console("cfg exec default");
    check(right_click_until(http, 0, 0, marker_on).has_value(), "profiles: default again");

    // The second module, installed by copying its folder.
    console("whereami");
    {
        const auto chat{ game_reply_with(client, game_server, "/whereami", "BOXTEST") };
        check(contains(chat, "BOXTEST: tile 5, 4 (world 12x9)"), fmt::format("modules: /whereami answered by the copied module ({})", chat.value_or("-")));
    }

    // Switched off in config/modules.json: its handlers stop at once.
    write_app_file("config/modules.json", R"({ "profile": "default", "modules": { "reachability": { "enabled": false } } })");
    console("cfg reload");
    // (The test module "crash" still listens to right clicks, but fails.)
    answer = right_click_until(http, 0, 4, [](const std::string& body) { return has(body, "\"action\":\"right_click\""); });
    check(contains(answer, "No module answered this right click") && !contains(answer, "Цель"), fmt::format("modules: switched off - reachability no longer answers right clicks ({})", answer.value_or("-")));
    {
        game_server.inbox.clear();
        input(client, "/reach 0 4");
        const bool forwarded{ pump_until([&] {
            for (const auto& data : game_server.inbox) {
                if (const auto text{ parse_text(data) }; text && has(*text, "/reach 0 4")) {
                    return true;
                }
            }
            return false;
        }, 3000ms) };
        check(forwarded, "modules: switched off - /reach is not a proxy command any more (goes to the server)");
        WalkLog log{};
        pump_for(300ms);
        collect(client, game_server, log);
        check(!log.said("[map]"), "modules: switched off - no answer from the module");
    }
    console("reach 0 4");

    write_app_file("config/modules.json", modules_json);
    console("cfg reload");
    answer = right_click_until(http, 0, 4, steps_on);
    check(answer && steps_on(*answer), "modules: switched on again without a restart");
    {
        const auto chat{ game_reply(client, game_server, "/reach 0 4") };
        check(contains(chat, "target reachable"), "modules: /reach works again");
    }
    client.inbox.clear();
    game_server.inbox.clear();
}

// Without modules/: right clicks are not answered, /reach goes to the server.
void run_without_modules(Endpoint& client, Endpoint& game_server, httplib::Client& http)
{
    const auto answer{ right_click(http, 2, 2) };
    check(contains(answer, "no module handles it"), fmt::format("no modules: right click is not answered ({})", answer.value_or("-")));

    game_server.inbox.clear();
    input(client, "/reach 0 4");
    const bool forwarded{ pump_until([&] {
        for (const auto& data : game_server.inbox) {
            if (const auto text{ parse_text(data) }; text && has(*text, "/reach 0 4")) {
                return true;
            }
        }
        return false;
    }, 3000ms) };
    check(forwarded, "no modules: /reach is not a proxy command (goes to the server)");
}

void run_reach_checks(Endpoint& client, Endpoint& game_server)
{
    httplib::Client http{ "127.0.0.1", MAP_PORT };
    http.set_connection_timeout(2);
    http.set_read_timeout(5);
    client.inbox.clear();
    game_server.inbox.clear();

    const auto reach_is = [](const std::optional<std::string>& answer, const std::string& code, const std::string& message) {
        return contains(answer, fmt::format("\"reach\":\"{}\"", code)) && contains(answer, fmt::format("\"message\":\"{}", message));
    };

    // Joining a new world: no map yet.
    send(client, text_packet(packet::NET_MESSAGE_GAME_MESSAGE, "action|join_request\nname|BOXTEST\ninvitedWorld|0\n"));
    check(receive(game_server).has_value(), "reach: join request forwarded");
    if (!g_modules) {
        run_without_modules(client, game_server, http);
        client.inbox.clear();
        game_server.inbox.clear();
        return;
    }
    {
        const auto answer{ right_click(http, 2, 2) };
        check(reach_is(answer, "no_data", "Нет данных") && contains(answer, "карта мира ещё не получена"), fmt::format("reach: before the world arrives: Нет данных ({})", answer.value_or("-")));
        const auto chat{ game_reply(client, game_server, "/reach 2 2") };
        check(contains(chat, "no data"), fmt::format("reach: /reach without a world says no data ({})", chat.value_or("-")));
    }

    // The world, but not your position yet.
    send(game_server, map_data_packet(build_box_world()));
    check(receive(client).has_value(), "reach: world data forwarded");
    {
        const auto answer{ right_click(http, 2, 2) };
        check(reach_is(answer, "no_data", "Нет данных") && contains(answer, "позиция игрока ещё не получена"), fmt::format("reach: no position yet: Нет данных ({})", answer.value_or("-")));
    }

    // You spawn inside the closed box (screenshot 1).
    const std::string spawn{
        "spawn|avatar\nnetID|5\nuserID|1005\ncolrect|0|0|20|30\nposXY|160|128\nname|`wE2EPlayer``\ncountry|ru\ninvis|0\nmstate|0\nsmstate|0\nonlineID|\ntype|local"
    };
    send(game_server, variant_packet(packet::PacketVariant{ std::string{ "OnSpawn" }, spawn }));
    check(receive_call(client, "OnSpawn").has_value(), "reach: local OnSpawn forwarded");
    check(wait_json(http, "/api/state", [](const std::string& body) {
        return body.find("\"world\":\"BOXTEST\"") != std::string::npos && body.find("\"tx\":5,\"ty\":4,\"me\":true") != std::string::npos;
    }).has_value(), "reach: you are at 5, 4 in BOXTEST");

    {
        bool all_unreachable{ true };
        for (const auto [x, y] : std::array<std::pair<int, int>, 6>{ { { 0, 0 }, { 0, 4 }, { 2, 4 }, { 8, 4 }, { 5, 7 }, { 11, 0 } } }) {
            const auto answer{ right_click(http, x, y) };
            const bool ok{ reach_is(answer, "unreachable", "Цель недоступна\"") };
            all_unreachable = all_unreachable && ok;
            if (!ok) {
                fmt::print("    {}, {}: {}\n", x, y, answer.value_or("-"));
            }
        }
        check(all_unreachable, "reach: closed box - every free tile outside: Цель недоступна");

        const auto inside{ right_click(http, 4, 3) };
        check(reach_is(inside, "reachable", "Цель доступна") && contains(inside, "От 5, 4 до 4, 3: 2 шага"), fmt::format("reach: closed box - a tile inside: Цель доступна, 2 steps ({})", inside.value_or("-")));

        const auto wall{ right_click(http, 3, 4) };
        check(reach_is(wall, "blocked", "Цель недоступна: в клетке блок") && contains(wall, "Rock (10)"), fmt::format("reach: the wall itself is rejected ({})", wall.value_or("-")));

        const auto right{ right_click(http, 13, 4) };
        check(reach_is(right, "out_of_bounds", "Цель недоступна: за пределами карты"), fmt::format("reach: beyond the right edge is rejected ({})", right.value_or("-")));
        const auto left{ right_click(http, -2, 4, -96) };
        check(reach_is(left, "out_of_bounds", "Цель недоступна: за пределами карты") && contains(left, "\"tile\":{\"x\":-2,\"y\":4}"), fmt::format("reach: left of the world (scrolled view) is rejected ({})", left.value_or("-")));
    }

    // The block on your left is broken (screenshot 2): the box has an opening.
    send(game_server, tile_change(3, 4, 18));
    check(wait_json(http, "/api/tiles", [](const std::string& body) {
        const auto fg{ json_numbers(body, "fg") };
        return fg.size() == BOX_W * BOX_H && fg[4 * BOX_W + 3] == 0;
    }).has_value(), "reach: the broken block shows up on the map");
    {
        const auto out{ right_click(http, 0, 4) };
        check(reach_is(out, "reachable", "Цель доступна") && contains(out, "От 5, 4 до 0, 4: 5 шагов"), fmt::format("reach: opened box - outside through the opening: Цель доступна, 5 steps ({})", out.value_or("-")));

        const auto around{ right_click(http, 8, 4) };
        check(reach_is(around, "reachable", "Цель доступна"), fmt::format("reach: opened box - behind the far wall, around through the opening ({})", around.value_or("-")));

        const auto room{ right_click(http, 10, 3) };
        check(reach_is(room, "unreachable", "Цель недоступна\""), fmt::format("reach: another closed room stays unreachable ({})", room.value_or("-")));

        const auto pocket{ right_click(http, 10, 7) };
        check(reach_is(pocket, "unreachable", "Цель недоступна\""), fmt::format("reach: a pocket open only between two corners is unreachable ({})", pocket.value_or("-")));

        const auto block{ right_click(http, 1, 2) };
        check(reach_is(block, "blocked", "Цель недоступна: в клетке блок"), fmt::format("reach: a block outside is still rejected ({})", block.value_or("-")));

        const auto chat{ game_reply(client, game_server, "/reach 0 4") };
        check(contains(chat, "target reachable"), fmt::format("reach: /reach 0 4 in the game ({})", chat.value_or("-")));
        const auto chat_room{ game_reply(client, game_server, "/reach 10 3") };
        check(contains(chat_room, "target unreachable"), fmt::format("reach: /reach 10 3 in the game ({})", chat_room.value_or("-")));
    }

    // Only a check: nothing was sent to the server, you did not move.
    pump_for(200ms);
    WalkLog log{};
    collect(client, game_server, log);
    check(log.states.empty() && log.other_to_server.empty() && log.set_pos.empty(), "reach: checks never move you or send anything");

    run_profile_checks(client, game_server, http);

    // E2E_MAP_HOLD_SECONDS=N keeps you in BOXTEST for N seconds, e.g. to try
    // the map window in a browser while the fake server answers.
    if (const char* hold{ std::getenv("E2E_MAP_HOLD_SECONDS") }) {
        fmt::print("Holding the map world for {} s\n", hold);
        std::fflush(stdout);
        pump_for(std::chrono::seconds{ std::atoi(hold) });
    }

    client.inbox.clear();
    game_server.inbox.clear();
}
}

int main(const int argc, char** argv)
{
    if (argc < 2) {
        fmt::print("usage: {} <items.dat>\n", argv[0]);
        return 2;
    }

    const std::string items_dat_path{ argv[1] };
    if (const char* off{ std::getenv("E2E_NO_MODULES") }; off && std::string_view{ off } == "1") {
        g_modules = false;
    }
    if (const char* app_dir{ std::getenv("E2E_APP_DIR") }) {
        g_app_dir = app_dir;
    }
    const bool hash_mismatch{ argc > 2 && std::string_view{ argv[2] } == "mismatch" };

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

    // 4. Logon accept with a *changed* name suffix and the items.dat hash, sent
    //    as an unsigned integer like the official server does.
    const auto logon_hash{ static_cast<std::uint32_t>(items_hash) + (hash_mismatch ? 1u : 0u) };
    const packet::PacketVariant logon{
        std::string{ "OnSuperMainStartAcceptLogonFB211131ddf" },
        logon_hash,
        std::string{ "ubistatic-a.akamaihd.net" },
        std::string{ "0098/e2e/cache/" },
        std::string{ "cc.cz.madkite.freedom org.aqua.gg idv.aqua.bulldog" },
        std::string{ E2E_LOGON_SETTINGS },
        std::uint32_t{ 1234 }
    };
    const Bytes logon_packet{ variant_packet(logon) };
    send(game_server, logon_packet);
    if (!hash_mismatch) {
        const auto got{ receive(client) };
        check(got && same_payload(logon_packet, *got), "OnSuperMainStartAcceptLogon (new suffix) forwarded unchanged");
    } else {
        const auto got{ receive(client) };
        const auto variant{ got ? parse_variant(*got) : std::nullopt };
        check(variant.has_value(), "patched logon packet reaches the client");
        if (variant) {
            const auto& args{ variant->get_variants() };
            check(
                args.size() == logon.size() && variant->get<std::string>(0) == "OnSuperMainStartAcceptLogonFB211131ddf",
                "patched logon keeps name and all arguments"
            );
            check(
                args.size() > 1 && std::holds_alternative<std::uint32_t>(args[1]) && variant->get<std::uint32_t>(1) == 0u,
                "items.dat hash replaced with 0 (unsigned) so the client downloads items.dat"
            );
            bool others_unchanged{ args.size() == logon.size() };
            for (std::size_t i = 0; others_unchanged && i < args.size(); ++i) {
                others_unchanged = i == 1 || args[i] == logon.get_variants()[i];
            }
            check(others_unchanged, "all other logon arguments unchanged (CDN, settings string, tribute hash)");
            check(
                args.size() > 5 && variant->get<std::string>(5) == E2E_LOGON_SETTINGS,
                "session settings (enableInventoryTab/bigBackpack) intact"
            );
        }
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
        if (!g_modules && command == "/rainbow") {
            // An empty core: /rainbow belongs to modules/looks.
            const Bytes chat{ text_packet(packet::NET_MESSAGE_GENERIC_TEXT, "action|input\n|text|" + command) };
            send(client, chat);
            const auto got{ receive(game_server) };
            check(got && same_payload(chat, *got), "no modules: /rainbow is not a proxy command (goes to the server)");
            client.inbox.clear();
            game_server.inbox.clear();
            continue;
        }
        send(client, text_packet(packet::NET_MESSAGE_GENERIC_TEXT, "action|input\n|text|" + command));
        const auto reply{ receive(client, 3000ms) };
        pump_for(300ms);
        check(reply.has_value(), fmt::format("proxy answers {} itself", command));
        const auto reply_call{ reply ? parse_variant(*reply) : std::nullopt };
        const bool is_help{ command == "/proxyhelp" };
        check(
            reply_call && reply_call->get<std::string>(0) == (is_help ? "OnDialogRequest" : "OnConsoleMessage")
                && (!is_help || reply_call->get<std::string>(1).find("end_dialog|gtproxy_commands|") != std::string::npos),
            fmt::format("{} reply is {}", command, is_help ? "the command list window" : "an OnConsoleMessage the game prints")
        );
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

    if (g_modules) {
        // 8b. Proxy features (modules): nick, netIDs, the /proxy menu, warp and back.
        run_feature_checks(client, game_server, lock_item_id);

        // 8c. Item tools (modules/drop): /dd, /dw, /dropall, fast drop.
        run_item_checks(client, game_server, lock_item_id);
    }
    else {
        // 8b. Without modules: an empty core that only forwards.
        run_empty_core_checks(client, game_server, lock_item_id);
    }

    // 8d. Map window: cursor -> tile, walking, /goto, /clickwalk.
    run_map_checks(client, game_server);

    // 8e. Right click: is the target reachable (closed box, opened box).
    run_reach_checks(client, game_server);

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
    check(
        pump_until([&] { return sub_server.connects == 2; }),
        "without new server_data/redirect the proxy reconnects to the last server"
    );

    enet_host_destroy(client.host);
    enet_host_destroy(game_server.host);
    enet_host_destroy(sub_server.host);
    enet_deinitialize();

    fmt::print("\n{} failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
