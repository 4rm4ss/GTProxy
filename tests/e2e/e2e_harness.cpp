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
#include <cstring>
#include <algorithm>
#include <deque>
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

    // /dd: Diamond Locks first (higher value), then World Locks.
    input(client, "/dd");
    {
        const auto dl{ serve_drop(client, game_server, 1796, 2) };
        check(dl == 2, "/dd drops all Diamond Locks (answers the server's dialog with 2)");
        const auto wl{ serve_drop(client, game_server, 242, 5) };
        check(wl == 5, "/dd then drops all World Locks (5)");
        const auto done{ receive_call(client, "OnConsoleMessage") };
        bool finished{ false };
        for (auto call{ done }; call; call = receive_call(client, "OnConsoleMessage", 1500ms)) {
            if (call->first.get<std::string>(1).find("Drop finished") != std::string::npos) {
                finished = true;
                break;
            }
        }
        check(finished, "/dd reports when it is done");
        check(receive_call(client, "OnDialogRequest", 300ms) == std::nullopt, "the drop dialogs answered by the proxy never pop up in the game");
    }

    // /dw 3 drops only 3 of 5 World Locks.
    send(game_server, inventory);
    std::ignore = receive(client);
    input(client, "/dw 3");
    check(serve_drop(client, game_server, 242, 5) == 3, "/dw 3 answers the dialog with 3");
    pump_for(300ms);
    client.inbox.clear();

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

int main(const int argc, char** argv)
{
    if (argc < 2) {
        fmt::print("usage: {} <items.dat>\n", argv[0]);
        return 2;
    }

    const std::string items_dat_path{ argv[1] };
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
        send(client, text_packet(packet::NET_MESSAGE_GENERIC_TEXT, "action|input\n|text|" + command));
        const auto reply{ receive(client, 3000ms) };
        pump_for(300ms);
        check(reply.has_value(), fmt::format("proxy answers {} itself", command));
        const auto reply_call{ reply ? parse_variant(*reply) : std::nullopt };
        check(
            reply_call && reply_call->get<std::string>(0) == "OnConsoleMessage",
            fmt::format("{} reply is an OnConsoleMessage the game prints", command)
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

    // 8b. Proxy features: nick, netIDs, the /proxy menu, warp and back.
    run_feature_checks(client, game_server, lock_item_id);

    // 8c. Item tools: /dd, /dw, /dropall, fast drop.
    run_item_checks(client, game_server, lock_item_id);

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
