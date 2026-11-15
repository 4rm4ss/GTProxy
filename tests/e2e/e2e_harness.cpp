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
#include <map>
#include <cctype>
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
// hostname mode: config.json names www.growtopia1.com while the game server
// is the fake one here - admin mode must still work (the connected server decides).
bool g_official{ false };

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
            // Top corners: a Door (12) and the main White Door (6), with the
            // door data the server sends (label).
            const std::uint16_t door{ y == 0 && x == 0 ? std::uint16_t{ 12 } : y == 0 && x == MAP_W - 1 ? std::uint16_t{ 6 } : std::uint16_t{ 0 } };
            bs.write<std::uint16_t>(door != 0 ? door : map_fg(x, y));
            bs.write<std::uint16_t>(14);
            bs.write<std::uint16_t>(0);
            bs.write<std::uint16_t>(door != 0 ? 0x0001 : 0);   // Extra
            if (door != 0) {
                bs.write<std::uint8_t>(1);                    // door data
                bs.write(std::string{ door == 6 ? "EXIT" : "BACK" });
                bs.write<std::uint8_t>(0);
            }
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
            check(
                fg.size() == 96 && fg[0] == 12 && fg[11] == 6 && cell.size() == 96 && cell[0] == 5 && cell[11] == 4,
                fmt::format("map: a door in a world without a lock is walked through (cell 5), the main door (EXIT) only entered (cell 4) - got {}, {}",
                    cell.size() == 96 ? cell[0] : -1, cell.size() == 96 ? cell[11] : -1)
            );
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

// The Walking page of /proxy (modules/walk): its Save writes
// config/modules/walk.json like a hand edit and the walking engine follows
// at once; doors and entrances follow the "doors" setting.
void run_walking_settings_checks(Endpoint& client, Endpoint& game_server, httplib::Client& http)
{
    client.inbox.clear();
    game_server.inbox.clear();
    const auto walking_page = [&](const std::string& fields, const std::string& button) {
        send(client, text_packet(packet::NET_MESSAGE_GENERIC_TEXT,
            "action|dialog_return\ndialog_name|gtproxy_walking\n" + fields + "buttonClicked|" + button + "\n"));
        WalkLog log{};
        follow_walk(client, game_server, log, "Walking:", 3000ms);
        return log;
    };

    input(client, "/walking");
    {
        const auto call{ receive_call(client, "OnDialogRequest") };
        const std::string dialog{ call ? call->first.get<std::string>(1) : "" };
        check(
            dialog.find("end_dialog|gtproxy_walking|") != std::string::npos && dialog.find("add_text_input|speed|") != std::string::npos
                && dialog.find("add_text_input|air_jumps|") != std::string::npos && dialog.find("add_button|walk_doors_all|") != std::string::npos,
            "walking: /walking opens the Walking page (speed, jumps in the air, doors)"
        );
    }

    {
        const auto log{ walking_page("speed|900\nstep_ms|50\ngravity|0\njump_height|5\nair_jumps|1000\n", "walk_save") };
        check(log.said("900 px/s") && log.said("(saved)"), fmt::format("walking: Save on the page answers with the new settings ({})", log.console.empty() ? "-" : log.console.back()));
        const auto file{ read_app_file("config/modules/walk.json") };
        check(
            file.find("\"speed\": 900") != std::string::npos && file.find("\"air_jumps\": 1000") != std::string::npos
                && file.find("\"step_ms\": 50") != std::string::npos && file.find("\"tell_game\": true") != std::string::npos,
            fmt::format("walking: the page wrote config/modules/walk.json, other settings kept ({})", file)
        );
        check(game_server.inbox.empty(), "walking: the page is not sent to the server");
    }

    // An entrance in the wall of the closed room: a House Entrance in a
    // world without a lock lets you through.
    {
        utils::ByteStream<> tile{};
        tile.write<std::uint16_t>(224);     // House Entrance
        tile.write<std::uint16_t>(14);
        tile.write<std::uint16_t>(0);
        tile.write<std::uint16_t>(0);
        packet::GameUpdatePacket header{};
        header.type = packet::PACKET_SEND_TILE_UPDATE_DATA;
        header.int_x = 9;
        header.int_y = 3;
        send(game_server, game_packet(header, tile.take_data()));
        const auto room{ right_click_until(http, 10, 3, [](const std::string& body) { return has(body, "\"reach\":\"reachable\""); }) };
        check(contains(room, "\"reach\":\"reachable\""), fmt::format("walking: a House Entrance in an unlocked world opens the closed room ({})", room.value_or("-")));
    }

    {
        walking_page("", "walk_doors_none");
        const auto file{ read_app_file("config/modules/walk.json") };
        check(file.find("\"doors\": \"none\"") != std::string::npos, "walking: the doors buttons write config/modules/walk.json");
        const auto room{ right_click_until(http, 10, 3, [](const std::string& body) { return has(body, "\"reach\":\"unreachable\""); }) };
        check(contains(room, "\"reach\":\"unreachable\""), fmt::format("walking: doors none - the room is closed again ({})", room.value_or("-")));
        const auto walk{ click_tile(http, 10, 3, 0) };
        check(
            contains(walk, "the way goes through House Entrance (224) at 9, 3") && contains(walk, "doors: none"),
            fmt::format("walking: no way - the answer names the entrance that blocks it and why ({})", walk.value_or("-"))
        );
    }

    // Back to the defaults for the rest of the session.
    walking_page("", "walk_doors_access");
    walking_page("speed|250\nstep_ms|100\ngravity|0\njump_height|3\nair_jumps|1\n", "walk_save");
    client.inbox.clear();
    game_server.inbox.clear();
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
    run_walking_settings_checks(client, game_server, http);

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

// --- Test stand: the five modules of the local test world --------------------
// FINDTEST: the box of BOXTEST with things lying around (item_finder,
// world_scan). SHOPTEST: vending machines, a lock and other players
// (vend_finder, world_moderation, autohost). The fake server answers like
// the game's: the vending machine's window, the player's window (wrench),
// the lock's window and the drop window.

// A dropped object appears (-1), changes (-3) or is collected by a player
// (the player's netID), as the game sends it.
Bytes object_change(const std::int32_t change, const std::uint32_t object_id, const int item, const int amount, const int tx, const int ty)
{
    packet::GameUpdatePacket header{};
    header.type = packet::PACKET_ITEM_CHANGE_OBJECT;
    header.net_id = static_cast<std::uint32_t>(change);
    header.item_net_id = static_cast<std::int32_t>(object_id);
    header.float_var = static_cast<float>(amount);
    header.int_data = item;
    header.pos_x = static_cast<float>(tx * 32 + 6);
    header.pos_y = static_cast<float>(ty * 32 + 6);
    return game_packet(header, {});
}

struct StandObject {
    std::uint16_t item{ 0 };
    int x{ 0 };
    int y{ 0 };
    std::uint8_t amount{ 0 };
};

void write_lock_extra(utils::ByteStream<>& bs, const std::uint32_t owner, const std::vector<std::uint32_t>& access)
{
    bs.write<std::uint8_t>(3);                  // TileExtra::Type::Lock
    bs.write<std::uint8_t>(0);
    bs.write<std::uint32_t>(owner);
    bs.write<std::uint32_t>(static_cast<std::uint32_t>(access.size()));
    for (const auto id : access) {
        bs.write<std::uint32_t>(id);
    }
    for (int j = 0; j < 8; ++j) {
        bs.write<std::uint8_t>(0);
    }
}

// A world: fg(x, y) for blocks, extra(x, y, bs) writes a tile's extra data
// and returns true when it did.
Bytes build_stand_world(
    const std::string& name, const int width, const int height,
    const std::function<std::uint16_t(int, int)>& fg,
    const std::function<bool(int, int, utils::ByteStream<>&)>& extra,
    const std::vector<StandObject>& objects
)
{
    utils::ByteStream<> bs{};
    bs.write<std::uint16_t>(0x18);
    bs.write<std::uint32_t>(0);
    bs.write(name);
    bs.write<std::int32_t>(width);
    bs.write<std::int32_t>(height);
    bs.write<std::uint32_t>(static_cast<std::uint32_t>(width * height));
    for (int i = 0; i < 5; ++i) {
        bs.write<std::uint8_t>(0);
    }
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            utils::ByteStream<> tail{};
            const bool has_extra{ extra(x, y, tail) };
            bs.write<std::uint16_t>(fg(x, y));
            bs.write<std::uint16_t>(14);
            bs.write<std::uint16_t>(0);
            bs.write<std::uint16_t>(has_extra ? 1 : 0);
            if (has_extra) {
                const auto data{ tail.take_data() };
                for (const auto b : data) {
                    bs.write<std::uint8_t>(static_cast<std::uint8_t>(b));
                }
            }
        }
    }
    for (int i = 0; i < 12; ++i) {
        bs.write<std::uint8_t>(0);
    }
    bs.write<std::uint32_t>(static_cast<std::uint32_t>(objects.size()));
    bs.write<std::uint32_t>(static_cast<std::uint32_t>(objects.size()));
    std::uint32_t id{ 0 };
    for (const auto& o : objects) {
        bs.write<std::uint16_t>(o.item);
        bs.write<float>(static_cast<float>(o.x * 32 + 6));
        bs.write<float>(static_cast<float>(o.y * 32 + 6));
        bs.write<std::uint8_t>(o.amount);
        bs.write<std::uint8_t>(0);
        bs.write<std::uint32_t>(++id);
    }
    return bs.take_data();
}

std::string spawn_at(const int net_id, const int user_id, const std::string& name, const int tx, const int ty, const bool local)
{
    return fmt::format(
        "spawn|avatar\nnetID|{}\nuserID|{}\ncolrect|0|0|20|30\nposXY|{}|{}\nname|`w{}``\ncountry|ru\ninvis|0\nmstate|0\nsmstate|0\nonlineID|{}",
        net_id, user_id, tx * 32, ty * 32, name, local ? "\ntype|local" : ""
    );
}

void spawn(Endpoint& client, Endpoint& game_server, const int net_id, const int user_id, const std::string& name, const int tx, const int ty, const bool local = false)
{
    send(game_server, variant_packet(packet::PacketVariant{ std::string{ "OnSpawn" }, spawn_at(net_id, user_id, name, tx, ty, local) }));
    std::ignore = receive_call(client, "OnSpawn");
}

void remove_player(Endpoint& game_server, const int net_id, const int user_id)
{
    send(game_server, variant_packet(packet::PacketVariant{
        std::string{ "OnRemove" }, fmt::format("netID|{}\n", net_id), fmt::format("pId|{}\n", user_id) }));
}

Bytes dialog_packet(const std::string& markup)
{
    return variant_packet(packet::PacketVariant{ std::string{ "OnDialogRequest" }, markup });
}

Bytes console_packet(const std::string& text)
{
    return variant_packet(packet::PacketVariant{ std::string{ "OnConsoleMessage" }, text });
}

std::string url_encode(const std::string& text)
{
    std::string out;
    for (const unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += fmt::format("%{:02X}", c);
        }
    }
    return out;
}

// A button of a module's tab in the map window; the module's answer.
std::optional<std::string> panel(
    httplib::Client& http, const std::string& key, const std::string& action,
    const std::string& row = {}, const std::string& value = {},
    const std::vector<std::pair<std::string, std::string>>& fields = {}
)
{
    std::string path{ fmt::format("/api/panel?panel={}&action={}&row={}&value={}", url_encode(key), url_encode(action), url_encode(row), url_encode(value)) };
    for (const auto& [name, text] : fields) {
        path += fmt::format("&f_{}={}", url_encode(name), url_encode(text));
    }
    return map_post(http, path);
}

std::optional<std::string> modules_until(httplib::Client& http, const std::function<bool(const std::string&)>& ok, const std::chrono::milliseconds timeout = 3000ms)
{
    return wait_json(http, "/api/modules", ok, timeout);
}

// The spec of one tab of /api/modules (a module's own JSON), "" if none.
std::string tab_of(const std::string& modules, const std::string& key)
{
    const auto start{ modules.find(fmt::format("{{\"key\":\"{}\",\"spec\":", key)) };
    if (start == std::string::npos) {
        return {};
    }
    const auto next{ modules.find("{\"key\":\"", start + 10) };
    return modules.substr(start, next == std::string::npos ? std::string::npos : next - start);
}

std::optional<std::string> tab_until(httplib::Client& http, const std::string& key, const std::function<bool(const std::string&)>& ok, const std::chrono::milliseconds timeout = 3000ms)
{
    std::string last;
    const auto got{ modules_until(http, [&](const std::string& body) {
        last = tab_of(body, key);
        return !last.empty() && ok(last);
    }, timeout) };
    if (!got) {
        fmt::print("    tab {}: {}\n", key, last.substr(0, 1500));
        return std::nullopt;
    }
    return last;
}

// The next text packet the server gets that starts with `prefix`.
std::optional<std::string> server_text(Endpoint& game_server, const std::string& prefix, const std::chrono::milliseconds timeout = 3000ms)
{
    std::optional<std::string> found;
    pump_until([&] {
        while (!game_server.inbox.empty()) {
            const Bytes data{ std::move(game_server.inbox.front()) };
            game_server.inbox.pop_front();
            if (const auto text{ parse_text(data) }; text && text->starts_with(prefix)) {
                found = *text;
                return true;
            }
        }
        return false;
    }, timeout);
    return found;
}

// The next wrench (TILE_CHANGE_REQUEST with the wrench, 32) the server gets.
bool server_wrench(Endpoint& game_server, const int x, const int y, const std::chrono::milliseconds timeout = 3000ms)
{
    return pump_until([&] {
        while (!game_server.inbox.empty()) {
            const Bytes data{ std::move(game_server.inbox.front()) };
            game_server.inbox.pop_front();
            const auto header{ game_header(data) };
            if (header && header->type == packet::PACKET_TILE_CHANGE_REQUEST && header->int_x == x && header->int_y == y && header->int_data == 32) {
                return true;
            }
        }
        return false;
    }, timeout);
}

// Nothing of `kind` reached the game (a module kept the server's window).
bool client_got_call(Endpoint& client, const std::string& function, const std::chrono::milliseconds wait = 300ms)
{
    bool got{ false };
    pump_until([&] {
        while (!client.inbox.empty()) {
            const Bytes data{ std::move(client.inbox.front()) };
            client.inbox.pop_front();
            if (const auto variant{ parse_variant(data) }; variant && variant->get<std::string>(0) == function) {
                got = true;
            }
        }
        return got;
    }, wait);
    return got;
}

// Console lines the game shows after `command`, until one has `until`.
std::vector<std::string> game_lines(Endpoint& client, Endpoint& game_server, const std::string& command, const std::string& until)
{
    WalkLog log{};
    input(client, command);
    follow_walk(client, game_server, log, until, 3000ms);
    return log.console;
}

std::string joined(const std::vector<std::string>& lines)
{
    std::string text;
    for (const auto& line : lines) {
        text += line + "\n";
    }
    return text;
}

bool before(const std::string& text, const std::string& first, const std::string& second)
{
    const auto a{ text.find(first) };
    const auto b{ text.find(second) };
    return a != std::string::npos && b != std::string::npos && a < b;
}

void enter_world(Endpoint& client, Endpoint& game_server, httplib::Client& http, const std::string& name, const Bytes& world, const int tx, const int ty)
{
    send(client, text_packet(packet::NET_MESSAGE_GAME_MESSAGE, fmt::format("action|join_request\nname|{}\ninvitedWorld|0\n", name)));
    std::ignore = receive(game_server);
    send(game_server, map_data_packet(world));
    std::ignore = receive(client);
    spawn(client, game_server, 5, 1005, "E2EPlayer", tx, ty, true);
    check(wait_json(http, "/api/state", [&](const std::string& body) {
        return body.find(fmt::format("\"world\":\"{}\"", name)) != std::string::npos
            && body.find(fmt::format("\"tx\":{},\"ty\":{},\"me\":true", tx, ty)) != std::string::npos;
    }).has_value(), fmt::format("stand: you are at {}, {} in {}", tx, ty, name));
    pump_for(300ms);
    client.inbox.clear();
    game_server.inbox.clear();
}

void run_finder_flows(Endpoint& client, Endpoint& game_server, httplib::Client& http)
{
    // FINDTEST: BOXTEST's closed box (you at 5, 4); Dirt at 0, 4 (left,
    // 5 steps once the box opens), 8, 4 (right: closer in a straight line,
    // far by the way around) and 10, 7 (open only between two corners),
    // Gems inside the box, Rock in the closed room.
    const std::vector<StandObject> objects{ { 2, 0, 4, 3 }, { 2, 8, 4, 7 }, { 2, 10, 7, 1 }, { 112, 4, 5, 5 }, { 10, 10, 3, 2 } };
    enter_world(client, game_server, http, "FINDTEST",
        build_stand_world("FINDTEST", BOX_W, BOX_H, box_fg, [](int, int, utils::ByteStream<>&) { return false; }, objects), 5, 4);

    const std::string items{ "item_finder/items" };
    {
        const auto chat{ game_reply_with(client, game_server, "/pfind 2", "can be reached") };
        check(contains(chat, "3 matching item(s) lie here, but none can be reached"), fmt::format("item_finder: closed box - Dirt outside, none reachable ({})", chat.value_or("-")));
        const auto gems{ game_reply_with(client, game_server, "/pfind gems", "Nearest") };
        check(contains(gems, "Gems x5") && contains(gems, "4, 5") && contains(gems, "2 steps"), fmt::format("item_finder: by name - the Gems inside the box, 2 steps ({})", gems.value_or("-")));
    }
    {
        const auto chat{ joined(game_lines(client, game_server, "/growscan", "full table")) };
        check(has(chat, "`w40`` blocks, `w108`` backgrounds, on the ground `w4`` stacks / `w13`` pieces, gems `w5``"),
            fmt::format("world_scan: blocks, backgrounds, stacks / pieces and gems apart ({})", chat));
        check(tab_until(http, "world_scan/scan", [](const std::string& tab) {
            return has(tab, "гемы на земле: 5 (кучек 1)") && has(tab, "Rock (10)") && has(tab, "[\"Dirt (2)\",12,0,3,11]");
        }).has_value(), "world_scan: the table - Dirt: 12 blocks, 3 stacks / 11 pieces on the ground");
        const auto select{ panel(http, "world_scan/scan", "select", "2") };
        check(contains(select, "мест на карте 15"), fmt::format("world_scan: a row marks every place of the item ({})", select.value_or("-")));
        const auto track{ game_reply_with(client, game_server, "/track on", "journal") };
        check(contains(track, "Item journal `2on``"), fmt::format("world_scan: /track on ({})", track.value_or("-")));
    }

    // The block left of you is broken: the box opens on the left.
    send(game_server, tile_change(3, 4, 18));
    check(wait_json(http, "/api/tiles", [](const std::string& body) {
        const auto fg{ json_numbers(body, "fg") };
        return fg.size() == BOX_W * BOX_H && fg[4 * BOX_W + 3] == 0;
    }).has_value(), "stand: the box opens on the left");
    {
        const auto chat{ game_reply_with(client, game_server, "/pfind 2", "Nearest") };
        check(contains(chat, "Dirt x3`` at `w0, 4``, 5 steps (3 matching)"),
            fmt::format("item_finder: box opened left - the Dirt at 0, 4 (5 steps), not the one closer in a line behind the right wall ({})", chat.value_or("-")));
        const auto tab{ tab_until(http, items, [](const std::string& tab) {
            return has(tab, "Цель: Dirt x3 (ID 2) в 0, 4 — путь 5 шаг.") && has(tab, "[\"Dirt (2)\",1,\"10, 7\",\"нет пути\"]");
        }) };
        check(tab.has_value(), "item_finder: the tab - the target with its way, and the pocket open only diagonally has no way");
        const auto overlay{ modules_until(http, [](const std::string& body) {
            return has(body, "\"key\":\"item_finder/items\"") && has(body, "\"route\":[[5,4],[4,4],[3,4],[2,4],[1,4],[0,4]]");
        }) };
        check(overlay.has_value(), "item_finder: the way is drawn on the map, through the opening");
        check(tab_until(http, "world_scan/scan", [](const std::string& tab) { return has(tab, "Блоков: 39"); }).has_value(),
            "world_scan: the broken block is counted at once");
    }

    // Someone else picks the target up: the next nearest is chosen.
    spawn(client, game_server, 30, 1030, "Picker", 0, 4);
    send(game_server, object_change(30, 1, 2, 0, 0, 4));
    {
        const auto tab{ tab_until(http, items, [](const std::string& tab) {
            return has(tab, "поднял игрок Picker") && has(tab, "Цель: Dirt x7 (ID 2) в 8, 4");
        }) };
        check(tab.has_value(), "item_finder: the target vanished while searching - picked up by Picker, the next nearest (8, 4) chosen");
        check(tab_until(http, "world_scan/journal", [](const std::string& tab) { return has(tab, "Поднял игрок Picker: Dirt x3"); }).has_value(),
            "world_scan: /track names the player only when the server did");
    }
    send(game_server, object_change(77, 5, 0, 0, 10, 3));
    send(game_server, object_change(-1, 0, 112, 3, 6, 3));
    check(tab_until(http, "world_scan/journal", [](const std::string& tab) {
        return has(tab, "Объект исчез: Rock x2") && has(tab, "Появился на земле: Gems x3") && has(tab, "\"6, 3\"");
    }).has_value(), "world_scan: a stack gone with nobody known - Объект исчез; a new drop where it fell");
    check(tab_until(http, "world_scan/scan", [](const std::string& tab) { return has(tab, "гемы на земле: 8 (кучек 2)"); }).has_value(),
        "world_scan: the new gems are counted");

    // Any tile picked on the map.
    {
        check(contains(panel(http, items, "pick"), "Щёлкните клетку"), "item_finder: pick a tile on the map");
        const auto room{ click_tile(http, 10, 3, 0) };
        check(contains(room, "10, 3: пути нет"), fmt::format("item_finder: a tile in the closed room - no way ({})", room.value_or("-")));
        const auto go{ panel(http, items, "go") };
        check(contains(go, "До цели нет пути"), fmt::format("item_finder: no walking without a way ({})", go.value_or("-")));
        std::ignore = panel(http, items, "pick");
        const auto inside{ click_tile(http, 6, 5, 0) };
        check(contains(inside, "6, 5: путь 2 шаг."), fmt::format("item_finder: a tile in the box - its way ({})", inside.value_or("-")));
        const auto walk{ panel(http, items, "go") };
        check(contains(walk, "Идём к 6, 5") || contains(walk, "Ходьба недоступна"),
            fmt::format("item_finder: Идти к цели walks, or says why it cannot ({})", walk.value_or("-")));
        fmt::print("    item_finder go: {}\n", walk.value_or("-"));
        check(contains(panel(http, items, "stop"), "Стоп"), "item_finder: Стоп");
        pump_for(300ms);
        client.inbox.clear();
        game_server.inbox.clear();
    }

    // /render: a PNG named after the world and the date.
    {
        const auto saved{ game_reply_with(client, game_server, "/render", "Saved") };
        check(contains(saved, "FINDTEST_") && contains(saved, ".png"), fmt::format("world_scan: /render saves <WORLD>_<date>.png ({})", saved.value_or("-")));
        bool found{ false };
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator{ g_app_dir / "data" / "renders", ec }) {
            const auto name{ entry.path().filename().string() };
            found = found || (name.starts_with("FINDTEST_") && name.ends_with(".png") && entry.file_size() > 200);
        }
        check(found, "world_scan: the PNG is in data/renders");
    }
}

std::uint16_t shop_fg(const int x, const int y)
{
    if (y == 9) {
        return 10;
    }
    if (y == 8 && (x == 2 || x == 6 || x == 10 || x == 12 || x == 14)) {
        return 2978;
    }
    if (x == 0 && y == 0) {
        return 242;
    }
    return 0;
}

Bytes build_shop_world()
{
    return build_stand_world("SHOPTEST", 16, 10, shop_fg, [](const int x, const int y, utils::ByteStream<>& bs) {
        if (x == 0 && y == 0) {
            write_lock_extra(bs, 1005, { 2002 });
            return true;
        }
        if (y != 8 || shop_fg(x, y) != 2978) {
            return false;
        }
        const std::map<int, std::pair<std::uint32_t, std::int32_t>> machines{
            { 2, { 2, -3 } }, { 6, { 2, -2 } }, { 10, { 2, 1 } }, { 12, { 0, 0 } }, { 14, { 2, 0 } },
        };
        const auto [item, price]{ machines.at(x) };
        bs.write<std::uint8_t>(24);             // TileExtra::Type::Vending
        bs.write<std::uint32_t>(item);
        bs.write<std::uint32_t>(static_cast<std::uint32_t>(price));
        return true;
    }, {});
}

std::string vending_window(const int x, const int y, const int item, const int price, const std::vector<std::string>& lines)
{
    std::string texts;
    for (const auto& line : lines) {
        texts += fmt::format("add_textbox|{}|left|\n", line);
    }
    return fmt::format(
        "set_default_color|`o\nadd_label_with_icon|big|`wVending Machine``|left|2978|\n{}"
        "add_text_input|buycount|Buy how many?|0|5|\nembed_data|tilex|{}\nembed_data|tiley|{}\nembed_data|expectprice|{}\n"
        "embed_data|expectitem|{}\nend_dialog|vending|Cancel|Buy|\n", texts, x, y, price, item);
}

void run_vending_flows(Endpoint& client, Endpoint& game_server, httplib::Client& http)
{
    const std::string vend{ "vend_finder/vend" };
    {
        const auto lines{ joined(game_lines(client, game_server, "/pvend dirt", "14, 8")) };
        check(has(lines, "4 machine(s) with `wdirt``"), fmt::format("vend_finder: /pvend dirt - four machines sell Dirt ({})", lines));
        check(before(lines, "Dirt at `w2, 8``: 3 for 1 WL (0.3333 WL each)", "Dirt at `w6, 8``: 2 for 1 WL (0.5 WL each)")
            && before(lines, "6, 8``", "Dirt at `w10, 8``: 1 WL each (1 WL each)") && before(lines, "10, 8``", "Dirt at `w14, 8``: price unknown"),
            "vend_finder: cheapest per item first, two price forms compared per item, unknown price last");
        check(has(lines, "stock unknown"), "vend_finder: stock unknown until the machine's window is seen");
    }
    {
        const auto tab{ tab_until(http, vend, [](const std::string& tab) {
            return has(tab, "Цена неизвестна") && has(tab, "Остаток неизвестен") && before(tab, "\"id\":\"2,8\"", "\"id\":\"6,8\"")
                && before(tab, "\"id\":\"6,8\"", "\"id\":\"10,8\"") && before(tab, "\"id\":\"10,8\"", "\"id\":\"14,8\"");
        }) };
        check(tab.has_value(), "vend_finder: the tab - same order, Цена неизвестна / Остаток неизвестен");
        std::ignore = panel(http, vend, "search", {}, "");
        check(tab_until(http, vend, [](const std::string& tab) { return has(tab, "[\"пусто\",\"—\",\"—\",\"0\",\"12, 8\""); }).has_value(),
            "vend_finder: an empty machine is listed as empty with stock 0");
        std::ignore = panel(http, vend, "select", "14,8");
        const auto unknown{ panel(http, vend, "buy", {}, {}, { { "qty", "1" } }) };
        check(contains(unknown, "Цена неизвестна") && contains(unknown, "\"ok\":false"), fmt::format("vend_finder: no purchase at an unknown price ({})", unknown.value_or("-")));
    }
    {
        const auto select{ panel(http, vend, "select", "2,8") };
        check(contains(select, "2, 8: путь 1 шаг. до 3, 8"), fmt::format("vend_finder: a row - the way to a free tile next to the machine ({})", select.value_or("-")));
        check(modules_until(http, [](const std::string& body) { return has(body, "\"route\":[[4,8],[3,8]]") && has(body, "встать здесь"); }).has_value(),
            "vend_finder: the machine and the way are on the map");
        const auto buy{ panel(http, vend, "buy", {}, {}, { { "qty", "10" } }) };
        check(contains(buy, "12 шт. за 4 WL — подтвердите"), fmt::format("vend_finder: 10 at 3 per WL - whole packs: 12 for 4 WL, shown before confirming ({})", buy.value_or("-")));
        check(tab_until(http, vend, [](const std::string& tab) {
            return has(tab, "Ожидает подтверждения") && has(tab, "округлено вверх до целых пачек") && has(tab, "Подтвердить покупку");
        }).has_value(), "vend_finder: Ожидает подтверждения, and the rounding is said");
        game_server.inbox.clear();
        const auto confirm{ panel(http, vend, "confirm") };
        const auto twice{ panel(http, vend, "confirm") };
        check(contains(confirm, "Проверяем автомат"), fmt::format("vend_finder: confirm asks the machine again first ({})", confirm.value_or("-")));
        check(contains(twice, "Покупка уже выполняется"), fmt::format("vend_finder: a second click while it runs is refused ({})", twice.value_or("-")));
        check(server_wrench(game_server, 2, 8), "vend_finder: the machine is asked with the wrench, like by hand");
        send(game_server, dialog_packet(vending_window(2, 8, 2, -3, std::vector<std::string>{ "The machine contains a total of 115 Dirt.", "3 Dirt per World Lock." })));
        const auto answer{ server_text(game_server, "action|dialog_return") };
        check(answer == "action|dialog_return\ndialog_name|vending\ntilex|2|\ntiley|8|\nexpectprice|-3|\nexpectitem|2|\nbuycount|12\n",
            fmt::format("vend_finder: the same offer - one purchase of 12 sent with the machine's own data ({})", answer.value_or("-")));
        check(!client_got_call(client, "OnDialogRequest"), "vend_finder: the machine's window of the check does not pop up in the game");
        pump_for(300ms);
        check(game_server.inbox.empty(), "vend_finder: sent once, nothing more");
        send(game_server, inventory_packet({ { 2, 12, 0 }, { 242, 20, 0 } }));
        check(tab_until(http, vend, [](const std::string& tab) { return has(tab, "Подтверждено") && has(tab, "Получено 12 шт."); }).has_value(),
            "vend_finder: Подтверждено when the items arrive");
        check(tab_until(http, vend, [](const std::string& tab) { return has(tab, "[\"Dirt (2)\",\"3 for 1 WL\",\"0.3333\",\"115\""); }).has_value(),
            "vend_finder: the stock from the machine's window is shown");
    }
    {
        // The price changed after the choice: the new offer waits.
        std::ignore = panel(http, vend, "select", "6,8");
        const auto buy{ panel(http, vend, "buy", {}, {}, { { "qty", "4" } }) };
        check(contains(buy, "4 шт. за 2 WL"), fmt::format("vend_finder: 4 at 2 per WL ({})", buy.value_or("-")));
        game_server.inbox.clear();
        std::ignore = panel(http, vend, "confirm");
        check(server_wrench(game_server, 6, 8), "vend_finder: the machine is asked again before buying");
        send(game_server, dialog_packet(vending_window(6, 8, 2, -4, std::vector<std::string>{ "The machine contains a total of 40 Dirt.", "4 Dirt per World Lock." })));
        check(tab_until(http, vend, [](const std::string& tab) {
            return has(tab, "Предложение изменилось: цена 4 for 1 WL вместо 2 for 1 WL") && has(tab, "Ожидает подтверждения");
        }).has_value(), "vend_finder: the price changed after the choice - the new offer is shown for a new confirmation");
        check(!server_text(game_server, "action|dialog_return", 500ms).has_value(), "vend_finder: nothing bought at the changed price");
        check(contains(panel(http, vend, "cancel"), "Отменено"), "vend_finder: cancel");
    }
    {
        // No answer to the purchase: unknown, never sent again.
        std::ignore = panel(http, vend, "select", "10,8");
        std::ignore = panel(http, vend, "buy", {}, {}, { { "qty", "1" } });
        game_server.inbox.clear();
        std::ignore = panel(http, vend, "confirm");
        check(server_wrench(game_server, 10, 8), "vend_finder: 1 WL each - asked");
        send(game_server, dialog_packet(vending_window(10, 8, 2, 1, std::vector<std::string>{ "The machine contains a total of 5 Dirt.", "1 World Lock each." })));
        check(contains(server_text(game_server, "action|dialog_return"), "buycount|1\n"), "vend_finder: buying 1");
        check(tab_until(http, vend, [](const std::string& tab) { return has(tab, "Результат неизвестен"); }, 8000ms).has_value(),
            "vend_finder: no answer - Результат неизвестен");
        check(!server_text(game_server, "action|dialog_return", 300ms).has_value(), "vend_finder: and it is not sent again");
    }
    {
        // Leaving the world with a purchase waiting.
        std::ignore = panel(http, vend, "select", "2,8");
        std::ignore = panel(http, vend, "buy", {}, {}, { { "qty", "3" } });
        check(tab_until(http, vend, [](const std::string& tab) { return has(tab, "Ожидает подтверждения"); }).has_value(), "vend_finder: a purchase waits");
        send(game_server, variant_packet(packet::PacketVariant{ std::string{ "OnRequestWorldSelectMenu" }, std::string{ "default|\n" } }));
        check(tab_until(http, vend, [](const std::string& tab) { return has(tab, "Вы не в мире."); }).has_value(), "vend_finder: leaving the world closes it");
        enter_world(client, game_server, http, "SHOPTEST", build_shop_world(), 4, 8);
        const auto confirm{ panel(http, vend, "confirm") };
        check(contains(confirm, "Нечего подтверждать"), fmt::format("vend_finder: back in the world nothing is left to confirm ({})", confirm.value_or("-")));
    }
}

// The player's window (wrench) as the server sends it.
std::string player_window(const int net_id, const std::vector<std::string>& buttons)
{
    std::string markup{ fmt::format("set_default_color|`o\nadd_label_with_icon|big|`wPlayer``|left|18|\nembed_data|netID|{}\n", net_id) };
    for (const auto& b : buttons) {
        markup += fmt::format("add_button|{0}|`w{0}``|noflags|0|0|\n", b);
    }
    return markup + "end_dialog|popup||Continue|\n";
}

std::optional<std::string> wrench_player(Endpoint& game_server, const int net_id, const std::chrono::milliseconds timeout = 3000ms)
{
    return server_text(game_server, fmt::format("action|wrench\n|netid|{}\n", net_id), timeout);
}

Bytes lock_update(const std::vector<std::uint32_t>& access)
{
    utils::ByteStream<> tile{};
    tile.write<std::uint16_t>(242);
    tile.write<std::uint16_t>(14);
    tile.write<std::uint16_t>(0);
    tile.write<std::uint16_t>(1);
    write_lock_extra(tile, 1005, access);
    packet::GameUpdatePacket header{};
    header.type = packet::PACKET_SEND_TILE_UPDATE_DATA;
    header.int_x = 0;
    header.int_y = 0;
    return game_packet(header, tile.take_data());
}

void run_moderation_flows(Endpoint& client, Endpoint& game_server, httplib::Client& http)
{
    const std::string mod{ "world_moderation/moderation" };
    const std::string log_tab{ "world_moderation/moderation_log" };
    const auto journal_has = [&](const std::string& part, const std::chrono::milliseconds timeout = 3000ms) {
        return tab_until(http, log_tab, [&](const std::string& tab) { return has(tab, part); }, timeout).has_value();
    };
    check(tab_until(http, mod, [](const std::string& tab) { return has(tab, "\"Alice\",2020,20") && has(tab, "\"Bob\",2021,21"); }).has_value(),
        "world_moderation: the players with their account ID and netID");

    // Kick: the player's window, its Kick button, the player leaves.
    check(contains(panel(http, mod, "select", "20"), "Выбран Alice"), "world_moderation: choose Alice");
    check(contains(panel(http, mod, "kick"), "Kick для 1 — подтвердите"), "world_moderation: kick waits for confirmation");
    game_server.inbox.clear();
    std::ignore = panel(http, mod, "confirm");
    check(wrench_player(game_server, 20).has_value(), "world_moderation: the player's window is asked by netID (the wrench)");
    send(game_server, dialog_packet(player_window(20, { "pull", "kick", "worldban" })));
    const auto kick{ server_text(game_server, "action|dialog_return") };
    check(kick == "action|dialog_return\ndialog_name|popup\nnetID|20|\nbuttonClicked|kick\n", fmt::format("world_moderation: the window's own Kick button is pressed ({})", kick.value_or("-")));
    check(!client_got_call(client, "OnDialogRequest"), "world_moderation: the window does not pop up in the game");
    remove_player(game_server, 20, 2020);
    check(journal_has("\"SHOPTEST\",\"Kick\",\"Alice (#2020)\",\"Подтверждено\",\"игрок покинул мир\""), "world_moderation: Подтверждено once the player left");

    // No rights: no such button in the window.
    std::ignore = panel(http, mod, "select", "21");
    std::ignore = panel(http, mod, "ban");
    game_server.inbox.clear();
    std::ignore = panel(http, mod, "confirm");
    check(wrench_player(game_server, 21).has_value(), "world_moderation: Bob's window asked");
    send(game_server, dialog_packet(player_window(21, { "pull" })));
    check(journal_has("\"Ban\",\"Bob (#2021)\",\"Нет прав\",\"в окне игрока нет кнопки Ban\""), "world_moderation: no Ban button - Нет прав");
    check(!server_text(game_server, "action|dialog_return", 300ms).has_value(), "world_moderation: nothing pressed without rights");

    // The window never comes: the interface is not there.
    std::ignore = panel(http, mod, "pull");
    std::ignore = panel(http, mod, "confirm");
    check(wrench_player(game_server, 21).has_value(), "world_moderation: asked again for pull");
    check(journal_has("\"Pull\",\"Bob (#2021)\",\"Недоступно\",\"сервер не открыл окно игрока\"", 6000ms), "world_moderation: no window - Недоступно");

    // The server refuses.
    std::ignore = panel(http, mod, "kick");
    std::ignore = panel(http, mod, "confirm");
    check(wrench_player(game_server, 21).has_value(), "world_moderation: kick Bob");
    send(game_server, dialog_packet(player_window(21, { "kick" })));
    check(contains(server_text(game_server, "action|dialog_return"), "buttonClicked|kick"), "world_moderation: Kick pressed");
    send(game_server, console_packet("`4You can't kick Bob, they are a moderator."));
    check(journal_has("\"Kick\",\"Bob (#2021)\",\"Отклонено\",\"сервер: You can't kick Bob, they are a moderator.\""), "world_moderation: the server refused - Отклонено with its words");

    // No answer at all.
    std::ignore = panel(http, mod, "kick");
    std::ignore = panel(http, mod, "confirm");
    check(wrench_player(game_server, 21).has_value(), "world_moderation: kick Bob again");
    send(game_server, dialog_packet(player_window(21, { "kick" })));
    check(server_text(game_server, "action|dialog_return").has_value(), "world_moderation: Kick pressed again");
    check(journal_has("\"Kick\",\"Bob (#2021)\",\"Результат неизвестен\"", 7000ms), "world_moderation: no answer - Результат неизвестен");

    // Several at once; one leaves before its turn.
    spawn(client, game_server, 22, 2022, "Carol", 7, 8);
    spawn(client, game_server, 23, 2023, "Dave", 7, 7);
    std::ignore = panel(http, mod, "none");
    std::ignore = panel(http, mod, "select", "22");
    std::ignore = panel(http, mod, "select", "23");
    check(contains(panel(http, mod, "kick"), "Kick для 2"), "world_moderation: two chosen");
    game_server.inbox.clear();
    std::ignore = panel(http, mod, "confirm");
    check(wrench_player(game_server, 22).has_value(), "world_moderation: one after another - Carol first");
    remove_player(game_server, 23, 2023);
    send(game_server, dialog_packet(player_window(22, { "kick" })));
    check(server_text(game_server, "action|dialog_return").has_value(), "world_moderation: Carol kicked");
    remove_player(game_server, 22, 2022);
    check(journal_has("\"Carol (#2022)\",\"Подтверждено\"") && journal_has("\"Dave (#2023)\",\"Пропущено\",\"игрок ушёл из мира\""),
        "world_moderation: each with its own result - Dave left before his turn: Пропущено");
    check(!wrench_player(game_server, 23, 300ms).has_value(), "world_moderation: nothing sent for a player who left");

    // Lock access, through the lock's own window.
    check(contains(panel(http, mod, "lock"), "Щёлкните замок"), "world_moderation: pick a lock");
    const auto picked{ click_tile(http, 0, 0, 0) };
    check(contains(picked, "Замок 0, 0 выбран"), fmt::format("world_moderation: the lock picked on the map ({})", picked.value_or("-")));
    check(tab_until(http, mod, [](const std::string& tab) {
        return has(tab, "Замок 0, 0: World Lock (242), владелец E2EPlayer (#1005). Доступ: #2002. Сейчас здесь с доступом: никого.");
    }).has_value(), "world_moderation: where, what kind, owner and who has access");
    std::ignore = panel(http, mod, "none");
    std::ignore = panel(http, mod, "select", "21");
    std::ignore = panel(http, mod, "give");
    game_server.inbox.clear();
    std::ignore = panel(http, mod, "confirm");
    check(server_wrench(game_server, 0, 0), "world_moderation: the lock's window asked (wrench on the lock)");
    const std::string lock_window{
        "add_label_with_icon|big|`wEdit World Lock``|left|242|\nadd_checkbox|checkbox_2002|Zed|1\nadd_player_picker|playerNetID|`wAdd``|\n"
        "embed_data|tilex|0\nembed_data|tiley|0\nend_dialog|lock_edit|Cancel|OK|\n"
    };
    send(game_server, dialog_packet(lock_window));
    const auto give{ server_text(game_server, "action|dialog_return") };
    check(contains(give, "dialog_name|lock_edit\ntilex|0|\ntiley|0|\ncheckbox_2002|1\nplayerNetID|21\n"), fmt::format("world_moderation: Bob added with the lock's player picker ({})", give.value_or("-")));
    send(game_server, lock_update({ 2002, 2021 }));
    check(journal_has("\"Дать доступ\",\"Bob (#2021)\",\"Подтверждено\",\"доступ есть\""), "world_moderation: access given - Подтверждено from the lock itself");
    std::ignore = panel(http, mod, "take");
    std::ignore = panel(http, mod, "confirm");
    check(server_wrench(game_server, 0, 0), "world_moderation: the lock's window asked again");
    send(game_server, dialog_packet(
        "add_checkbox|checkbox_2002|Zed|1\nadd_checkbox|checkbox_2021|Bob|1\nadd_player_picker|playerNetID|`wAdd``|\n"
        "embed_data|tilex|0\nembed_data|tiley|0\nend_dialog|lock_edit|Cancel|OK|\n"));
    const auto take{ server_text(game_server, "action|dialog_return") };
    check(contains(take, "checkbox_2002|1\ncheckbox_2021|0\n"), fmt::format("world_moderation: Bob's box unticked, the others kept ({})", take.value_or("-")));
    send(game_server, lock_update({ 2002 }));
    check(journal_has("\"Забрать доступ\",\"Bob (#2021)\",\"Подтверждено\",\"доступа больше нет\""), "world_moderation: access taken - Подтверждено");

    // Entry rules: a dry run first, a broken rule keeps the last good ones.
    const auto settings{ read_app_file("config/modules/world_moderation.json") };
    write_app_file("config/modules/world_moderation.json",
        R"({ "rules_enabled": true, "dry_run": true, "rules": "world=SHOPTEST action=ban name=BadGuy", "exceptions": "name=Bob" })");
    console("cfg reload");
    check(tab_until(http, mod, [](const std::string& tab) { return has(tab, "Правила входа: включены, только проба (dry run), правил 1."); }).has_value(),
        "world_moderation: rules on, dry run (cfg reload)");
    game_server.inbox.clear();
    spawn(client, game_server, 24, 2024, "BadGuy", 11, 8);
    check(journal_has("\"Ban\",\"BadGuy (#2024)\",\"Проба (dry run)\""), "world_moderation: BadGuy enters - the dry run only writes it down");
    check(!wrench_player(game_server, 24, 500ms).has_value(), "world_moderation: nothing sent in a dry run");
    write_app_file("config/modules/world_moderation.json", R"({ "rules_enabled": true, "dry_run": true, "rules": "world=SHOPTEST action=explode name=x" })");
    console("cfg reload");
    check(tab_until(http, mod, [](const std::string& tab) {
        return has(tab, "config/modules/world_moderation.json: rules: rule 1") && has(tab, "действуют прежние правила") && has(tab, "правил 1.");
    }).has_value(), "world_moderation: a broken rule is named with its file and field; the last good rules stay");
    write_app_file("config/modules/world_moderation.json", settings);
    console("cfg reload");
    check(tab_until(http, mod, [](const std::string& tab) { return has(tab, "Правила входа: выключены"); }).has_value(), "world_moderation: rules off again");
    remove_player(game_server, 24, 2024);
    pump_for(300ms);
    client.inbox.clear();
    game_server.inbox.clear();
}

void run_autohost_flows(Endpoint& client, Endpoint& game_server, httplib::Client& http)
{
    const std::string host{ "autohost/autohost" };
    const std::string pay{ "autohost/payout" };
    check(contains(game_reply_with(client, game_server, "/win1 100", "Oops"), "point 1 is not set"), "autohost: no payout without point 1");
    check(contains(game_reply_with(client, game_server, "/host start", "Oops"), "points 1, 2 are needed first"), "autohost: start needs points 1 and 2");
    check(contains(game_reply_with(client, game_server, "/pos1", "Point 1"), "Point 1 set to `w4, 8`` (zone 3,7 - 5,9)"), "autohost: /pos1 takes your tile, with a zone around it");
    check(contains(panel(http, host, "pick", "2"), "Щёлкните клетку"), "autohost: point 2 picked on the map");
    check(contains(click_tile(http, 8, 8, 0), "Точка 2: 8, 8"), "autohost: point 2 at 8, 8");
    const auto start{ game_reply_with(client, game_server, "/host start", "Hosting") };
    check(contains(start, "Hosting in SHOPTEST: points 1 at 4,8, 2 at 8,8; commission 10%, down. Nothing is given or moved by starting."),
        fmt::format("autohost: start shows the world, the points and the commission ({})", start.value_or("-")));
    pump_for(200ms);
    check(game_server.inbox.empty(), "autohost: starting sends nothing to the server");

    spawn(client, game_server, 25, 2025, "Eve", 5, 8);
    send(game_server, inventory_packet({ { 2, 12, 0 }, { 242, 150, 0 } }));
    pump_for(200ms);
    const auto win{ game_reply_with(client, game_server, "/win1 100", "Payout") };
    check(contains(win, "Payout point 1: 100 WL - 10% = `w90 WL``"), fmt::format("autohost: /win1 100 - 90 WL after 10% ({})", win.value_or("-")));
    check(tab_until(http, pay, [](const std::string& tab) {
        return has(tab, "Получатель: Eve (#2025)") && has(tab, "100 WL × 10% = 10 → удержано 10, к выплате 90 WL")
            && has(tab, "комиссия округляется вниз (в пользу игрока)") && has(tab, "Ожидает подтверждения");
    }).has_value(), "autohost: the payout window - point, receiver, item, amount, commission, held back, paid, rounding rule");
    game_server.inbox.clear();
    const auto confirm{ panel(http, pay, "confirm") };
    check(contains(confirm, "Выплата отправлена"), fmt::format("autohost: confirmed ({})", confirm.value_or("-")));
    check(server_text(game_server, "action|drop\n|itemID|242\n").has_value(), "autohost: one ordinary drop of World Locks");
    send(game_server, drop_dialog(242, 150));
    const auto answer{ server_text(game_server, "action|dialog_return") };
    check(answer == "action|dialog_return\ndialog_name|drop_item\nitemID|242|\ncount|90\n", fmt::format("autohost: the drop window answered with 90, not all 150 ({})", answer.value_or("-")));
    send(game_server, object_change(-1, 0, 242, 90, 4, 8));
    check(tab_until(http, pay, [](const std::string& tab) { return has(tab, "Подтверждено") && has(tab, "на земле 90 WL в 4, 8"); }).has_value(),
        "autohost: Подтверждено when the 90 WL lie on the ground");

    // Too far from the point: not paid, the way is offered.
    std::ignore = panel(http, pay, "close");
    std::ignore = game_reply_with(client, game_server, "/win2 50", "Payout");
    game_server.inbox.clear();
    const auto far{ panel(http, pay, "confirm") };
    check(contains(far, "вы далеко от точки (вы 4, 8; точка 8, 8)") && contains(far, "Идти к точке"), fmt::format("autohost: too far from point 2 - not paid, the way offered ({})", far.value_or("-")));
    check(!server_text(game_server, "action|drop", 300ms).has_value(), "autohost: nothing dropped when too far");
    std::ignore = panel(http, pay, "close");

    // Kick everyone in point 1's zone through world_moderation.
    std::ignore = panel(http, host, "select", "1");
    const auto plan{ panel(http, host, "mod_kick") };
    check(contains(plan, "kick: 1 в зоне — подтвердите"), fmt::format("autohost: the players in the zone listed first ({})", plan.value_or("-")));
    check(tab_until(http, host, [](const std::string& tab) { return has(tab, "Ожидает подтверждения: kick в зоне точки 1 → Eve (#2025)"); }).has_value(),
        "autohost: who would be kicked is shown");
    game_server.inbox.clear();
    std::ignore = panel(http, host, "mod_confirm");
    check(wrench_player(game_server, 25).has_value(), "autohost: world_moderation asks Eve's window");
    send(game_server, dialog_packet(player_window(25, { "kick" })));
    check(server_text(game_server, "action|dialog_return").has_value(), "autohost: Kick pressed");
    remove_player(game_server, 25, 2025);
    check(tab_until(http, host, [](const std::string& tab) { return has(tab, "kick в зоне точки 1: Eve: Подтверждено"); }).has_value(),
        "autohost: the result per player");

    check(contains(game_reply_with(client, game_server, "/host reset", "Again within"), "Again within 15 s to confirm"), "autohost: reset asks first");
    check(contains(game_reply_with(client, game_server, "/host reset", "cleared"), "Points of SHOPTEST cleared."), "autohost: reset after confirming");
    check(contains(game_reply_with(client, game_server, "/win1", "Oops"), "point 1 is not set"), "autohost: the points are gone");
}


// --- Admin mode, god mode and the in-game pages ------------------------------

Bytes character_state(const int net_id, const float speed, const float gravity, const int flags)
{
    packet::GameUpdatePacket header{};
    header.type = packet::PACKET_SET_CHARACTER_STATE;
    header.net_id = static_cast<std::uint32_t>(net_id);
    header.pad[1] = 0x80;
    header.pad[2] = 0x80;
    header.float_var = 125.0f;
    header.int_data = flags;
    header.pos_x = 1000.0f;
    header.pos_y = 100.0f;
    header.pos_x2 = speed;
    header.pos_y2 = gravity;
    return game_packet(header, {});
}

// Waits for game packets of a type and console lines at once.
struct Seen {
    std::vector<packet::GameUpdatePacket> packets;
    std::vector<std::string> console;
    std::vector<std::string> dialogs;
    std::vector<std::pair<std::uint32_t, glm::vec2>> set_pos;
    [[nodiscard]] bool said(const std::string_view text) const
    {
        return std::ranges::any_of(console, [&](const std::string& line) { return line.find(text) != std::string::npos; });
    }
};

void take_client(Endpoint& client, Seen& seen, const packet::PacketType type)
{
    while (!client.inbox.empty()) {
        Bytes data{ std::move(client.inbox.front()) };
        client.inbox.pop_front();
        if (const auto header{ game_header(data) }; header && header->type == type) {
            seen.packets.push_back(*header);
            continue;
        }
        if (const auto variant{ parse_variant(data) }) {
            const auto function{ variant->get<std::string>(0) };
            if (function == "OnConsoleMessage") {
                seen.console.push_back(variant->get<std::string>(1));
            } else if (function == "OnDialogRequest") {
                seen.dialogs.push_back(variant->get<std::string>(1));
            } else if (function == "OnSetPos") {
                seen.set_pos.emplace_back(net_id_of(data), variant->get<glm::vec2>(1));
            }
        }
    }
}

Seen command_with_state(Endpoint& client, const std::string& command, const std::string& said, const bool want_state)
{
    Seen seen{};
    input(client, command);
    pump_until([&] {
        take_client(client, seen, packet::PACKET_SET_CHARACTER_STATE);
        return seen.said(said) && (!want_state || !seen.packets.empty());
    }, 3000ms);
    return seen;
}

Seen press(Endpoint& client, const std::string& dialog, const std::string& button, const std::string& until, const std::string& fields = {})
{
    Seen seen{};
    send(client, text_packet(packet::NET_MESSAGE_GENERIC_TEXT, fmt::format("action|dialog_return\ndialog_name|{}\n{}buttonClicked|{}\n", dialog, fields, button)));
    pump_until([&] {
        take_client(client, seen, packet::PACKET_SET_CHARACTER_STATE);
        return std::ranges::any_of(seen.dialogs, [&](const std::string& d) { return d.find(until) != std::string::npos; }) || seen.said(until);
    }, 3000ms);
    return seen;
}

bool any_has(const std::vector<std::string>& texts, const std::string_view part)
{
    return std::ranges::any_of(texts, [&](const std::string& t) { return t.find(part) != std::string::npos; });
}

void run_admin_flows(Endpoint& client, Endpoint& game_server)
{
    client.inbox.clear();
    game_server.inbox.clear();

    // The server's character state reaches the game unchanged by default.
    send(game_server, character_state(5, 300.0f, 600.0f, 0));
    {
        Seen seen{};
        pump_until([&] { take_client(client, seen, packet::PACKET_SET_CHARACTER_STATE); return !seen.packets.empty(); }, 2000ms);
        check(!seen.packets.empty() && seen.packets[0].pos_x2 == 300.0f && seen.packets[0].int_data == 0, "admin: the server's character state passes unchanged by default");
    }


    {
        const auto seen{ command_with_state(client, "/speed 2", "Admin mode: `wspeed x2``", true) };
        check(seen.said("Admin mode: `wspeed x2``"), g_official
            ? "admin: config.json says www.growtopia1.com, but the game server is not Growtopia's - /speed 2 works"
            : "admin: /speed 2 answered");
        check(!seen.packets.empty() && seen.packets.back().pos_x2 == 600.0f && seen.packets.back().pos_y2 == 600.0f,
            fmt::format("admin: the game gets its state again at once, walking speed x2 ({})", seen.packets.empty() ? 0.0f : seen.packets.back().pos_x2));
    }
    {
        const auto seen{ command_with_state(client, "/dj", "double jump", true) };
        check(!seen.packets.empty() && (seen.packets.back().int_data & 2) != 0, "admin: /dj - double jump in the state");
        const auto jump{ command_with_state(client, "/jump 0.5", "gravity x0.5", true) };
        check(!jump.packets.empty() && jump.packets.back().pos_y2 == 300.0f, "admin: /jump 0.5 - gravity halved (higher jumps)");
    }
    send(game_server, character_state(5, 310.0f, 600.0f, 0));
    {
        Seen seen{};
        pump_until([&] { take_client(client, seen, packet::PACKET_SET_CHARACTER_STATE); return !seen.packets.empty(); }, 2000ms);
        check(!seen.packets.empty() && seen.packets[0].pos_x2 == 620.0f && (seen.packets[0].int_data & 2) != 0 && seen.packets[0].pos_y2 == 300.0f,
            "admin: a new state from the server gets the same abilities");
    }
    send(game_server, character_state(20, 300.0f, 600.0f, 0));
    {
        Seen seen{};
        pump_until([&] { take_client(client, seen, packet::PACKET_SET_CHARACTER_STATE); return !seen.packets.empty(); }, 2000ms);
        check(!seen.packets.empty() && seen.packets[0].pos_x2 == 300.0f, "admin: other players' states are left alone");
    }

    // God mode: the damage the game reports never reaches the server.
    std::ignore = command_with_state(client, "/god on", "god mode", false);
    game_server.inbox.clear();
    send(client, state_packet({ 128.0f, 256.0f }, 0));
    send(client, state_packet({ 130.0f, 256.0f }, (1u << 6) | (1u << 16)));
    {
        std::vector<packet::GameUpdatePacket> states;
        pump_until([&] {
            while (!game_server.inbox.empty()) {
                const Bytes data{ std::move(game_server.inbox.front()) };
                game_server.inbox.pop_front();
                if (const auto header{ game_header(data) }; header && header->type == packet::PACKET_STATE) {
                    states.push_back(*header);
                }
            }
            return states.size() >= 2;
        }, 2000ms);
        check(states.size() == 2 && (static_cast<std::uint32_t>(states[1].flags.value) & ((1u << 6) | (1u << 16))) == 0 && states[1].pos_x == 130.0f,
            "admin: god mode - lava / damage flags taken out of the state the server gets, the rest kept");
    }
    send(client, text_packet(packet::NET_MESSAGE_GENERIC_TEXT, "action|respawn_spike\n"));
    {
        Seen seen{};
        pump_until([&] { take_client(client, seen, packet::PACKET_SET_CHARACTER_STATE); return !seen.set_pos.empty(); }, 2000ms);
        check(!seen.set_pos.empty() && seen.set_pos[0].first == 5 && seen.set_pos[0].second == glm::vec2{ 128.0f, 256.0f },
            "admin: god mode - a death on spikes puts you back where you stood");
        check(!server_text(game_server, "action|respawn_spike", 500ms).has_value(), "admin: god mode - the death is not reported to the server");
    }
    {
        std::ignore = command_with_state(client, "/god off", "Admin mode", false);
        std::ignore = command_with_state(client, "/dj off", "Admin mode", true);
        std::ignore = command_with_state(client, "/jump 1", "Admin mode", true);
        const auto seen{ command_with_state(client, "/speed 1", "Admin mode: `wnormal``", true) };
        check(!seen.packets.empty() && seen.packets.back().pos_x2 == 310.0f && seen.packets.back().int_data == 0, "admin: back to normal - the server's own state again");
    }
    game_server.inbox.clear();
    send(client, text_packet(packet::NET_MESSAGE_GENERIC_TEXT, "action|respawn_spike\n"));
    check(server_text(game_server, "action|respawn_spike").has_value(), "admin: without god mode the game's respawn reaches the server");
}

void run_page_flows(Endpoint& client, Endpoint& game_server)
{
    client.inbox.clear();
    game_server.inbox.clear();
    {
        Seen seen{};
        input(client, "/proxy");
        pump_until([&] { take_client(client, seen, packet::PACKET_SET_CHARACTER_STATE); return !seen.dialogs.empty(); }, 3000ms);
        const auto menu{ seen.dialogs.empty() ? std::string{} : seen.dialogs[0] };
        check(has(menu, "page_admin") && has(menu, "page_autohost") && has(menu, "page_moderation") && has(menu, "page_vending")
            && has(menu, "page_finder") && has(menu, "page_scan"), "pages: /proxy offers Admin mode, Autohost, Moderation, Vending, Find items, World scan");
    }
    {
        const auto page{ press(client, "gtproxy_menu", "page_moderation", "end_dialog|gtproxy_mod") };
        check(any_has(page.dialogs, "wm_player_21") && any_has(page.dialogs, "Bob (#2021)"), "pages: Moderation lists the players");
        const auto player{ press(client, "gtproxy_mod", "wm_player_21", "wm_do_kick") };
        check(any_has(player.dialogs, "Bob (user ID 2021, netID 21)"), "pages: a player's page");
        const auto confirm{ press(client, "gtproxy_mod", "wm_do_kick", "wm_confirm") };
        check(any_has(confirm.dialogs, "Kick: Bob (#2021)?"), "pages: kick asks for confirmation first");
        game_server.inbox.clear();
        std::ignore = press(client, "gtproxy_mod", "wm_confirm", "end_dialog|gtproxy_mod");
        check(wrench_player(game_server, 21).has_value(), "pages: confirmed - the player's window is asked");
        send(game_server, dialog_packet(player_window(21, { "kick" })));
        check(contains(server_text(game_server, "action|dialog_return"), "buttonClicked|kick"), "pages: Kick pressed in the player's window");
        remove_player(game_server, 21, 2021);
        Seen result{};
        pump_until([&] { take_client(client, result, packet::PACKET_SET_CHARACTER_STATE); return result.said("Podtverzhdeno"); }, 3000ms);
        check(result.said("Moderation: ``Kick Bob (#2021) - Podtverzhdeno (igrok pokinul mir)"), "pages: the result comes to the game's chat (Latin letters)");
    }
    {
        // Wrench mode: your own wrench on a player kicks at once.
        std::ignore = press(client, "gtproxy_mod", "wm_wrench_kick", "Wrench mode: `wkick``");
        check(read_app_file("config/modules/world_moderation.json").find("\"wrench_mode\": \"kick\"") != std::string::npos, "pages: wrench mode saved to the config");
        spawn(client, game_server, 26, 2026, "Zed", 6, 7);
        client.inbox.clear();
        game_server.inbox.clear();
        send(game_server, dialog_packet(player_window(26, { "pull", "kick", "worldban" })));
        check(!client_got_call(client, "OnDialogRequest"), "pages: wrench mode - the player's window does not pop up");
        check(wrench_player(game_server, 26).has_value(), "pages: wrench mode - asked again through the checked steps");
        send(game_server, dialog_packet(player_window(26, { "pull", "kick", "worldban" })));
        check(contains(server_text(game_server, "action|dialog_return"), "netID|26|\nbuttonClicked|kick"), "pages: wrench mode - Kick pressed");
        remove_player(game_server, 26, 2026);
        Seen result{};
        pump_until([&] { take_client(client, result, packet::PACKET_SET_CHARACTER_STATE); return result.said("Zed"); }, 3000ms);
        check(result.said("Kick Zed (#2026) - Podtverzhdeno"), "pages: wrench mode - confirmed");
        std::ignore = press(client, "gtproxy_mod", "wm_wrench_off", "Wrench mode: `woff``");
    }
    {
        const auto page{ press(client, "gtproxy_menu", "page_autohost", "end_dialog|gtproxy_host") };
        check(any_has(page.dialogs, "ah_set_1") && any_has(page.dialogs, "Points needed: ``1, 2"), "pages: Autohost shows the points needed");
        const auto set{ press(client, "gtproxy_host", "ah_set_1", "point 1 = 4, 8") };
        check(set.said("Autohost: ``point 1 = 4, 8 (zone 3,7 - 5,9)"), "pages: Set point 1 here");
        const auto admin{ press(client, "gtproxy_menu", "page_admin", "end_dialog|gtproxy_admin") };
        check(any_has(admin.dialogs, "Your own server: `2on``") && any_has(admin.dialogs, "Game server: `w"), "pages: Admin mode says it works and on which game server");
        const auto vend{ press(client, "gtproxy_menu", "page_vending", "end_dialog|gtproxy_vend") };
        check(any_has(vend.dialogs, "Dirt at 2,8: 3 for 1 WL (0.3333 each)"), "pages: Vending lists the machines, cheapest first");
        const auto scan{ press(client, "gtproxy_menu", "page_scan", "end_dialog|gtproxy_scan") };
        check(any_has(scan.dialogs, "SHOPTEST, 16x10"), "pages: World scan");
        const auto walk{ press(client, "gtproxy_menu", "page_walking", "end_dialog|gtproxy_walking") };
        check(any_has(walk.dialogs, "Hold `walt`` and click a tile in the game"), "pages: Walking offers Alt + click");
    }
    client.inbox.clear();
    game_server.inbox.clear();
}

void run_stand_flows(Endpoint& client, Endpoint& game_server)
{
    httplib::Client http{ "127.0.0.1", MAP_PORT };
    http.set_connection_timeout(2);
    http.set_read_timeout(5);
    client.inbox.clear();
    game_server.inbox.clear();

    run_finder_flows(client, game_server, http);

    // A new world: nothing of the old one stays.
    enter_world(client, game_server, http, "SHOPTEST", build_shop_world(), 4, 8);
    spawn(client, game_server, 20, 2020, "Alice", 8, 8);
    spawn(client, game_server, 21, 2021, "Bob", 9, 8);
    check(tab_until(http, "item_finder/items", [](const std::string& tab) { return has(tab, "Цель не выбрана.") && has(tab, "лежит 0"); }).has_value(),
        "item_finder: a new world - no target, nothing of FINDTEST");
    check(tab_until(http, "world_scan/scan", [](const std::string& tab) { return has(tab, "Мир SHOPTEST") && !has(tab, "Gems") && has(tab, "кучек 0, всего 0 шт."); }).has_value(),
        "world_scan: a new world - a new scan, nothing of FINDTEST");
    check(tab_until(http, "world_scan/journal", [](const std::string& tab) { return has(tab, "мир SHOPTEST · записей 0"); }).has_value(),
        "world_scan: the journal starts again in the new world");

    run_vending_flows(client, game_server, http);
    spawn(client, game_server, 20, 2020, "Alice", 8, 8);
    spawn(client, game_server, 21, 2021, "Bob", 9, 8);
    run_moderation_flows(client, game_server, http);
    run_autohost_flows(client, game_server, http);
    run_admin_flows(client, game_server);
    run_page_flows(client, game_server);

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
    g_official = hash_mismatch;

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

    // 8f. Test stand: the five modules of the local test world.
    if (g_modules) {
        run_stand_flows(client, game_server);
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
