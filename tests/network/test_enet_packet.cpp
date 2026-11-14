// Captures the first UDP datagram the proxy's ENet client sends (the CONNECT
// to the game server) and checks the "new packet" header the Growtopia server
// expects. Runs with every compiler/platform, so a build that produces
// different bytes (e.g. MSVC vs GCC) is caught.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <enet/enet.h>
#include <gtest/gtest.h>

namespace {
std::uint16_t be16(const std::uint8_t* p)
{
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

std::string hex(const std::vector<std::uint8_t>& data)
{
    std::string out;
    char buf[4];
    for (const auto b : data) {
        std::snprintf(buf, sizeof(buf), "%02x", b);
        out += buf;
    }
    return out;
}

class EnetPacketTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_EQ(enet_initialize(), 0); }
    void TearDown() override { enet_deinitialize(); }
};
}

TEST_F(EnetPacketTest, ConnectPacketHasValidNewHeader)
{
    // Plain UDP socket standing in for the game server.
    ENetSocket server{ enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM) };
    ASSERT_NE(server, ENET_SOCKET_NULL);
    ENetAddress bind_address{};
    enet_address_set_host_ip(&bind_address, "127.0.0.1");
    bind_address.port = 0;
    ASSERT_EQ(enet_socket_bind(server, &bind_address), 0);
    ENetAddress server_address{};
    ASSERT_EQ(enet_socket_get_address(server, &server_address), 0);
    enet_address_set_host_ip(&server_address, "127.0.0.1");

    for (int attempt = 0; attempt < 3; ++attempt) {
        // Same setup as network::Client.
        ENetHost* client{ enet_host_create(nullptr, 1, 2, 0, 0) };
        ASSERT_NE(client, nullptr);
        ASSERT_EQ(enet_host_compress_with_range_coder(client), 0);
        client->checksum = enet_crc32;
        client->usingNewPacket = 1;

        ASSERT_NE(enet_host_connect(client, &server_address, 2, 0), nullptr);
        enet_host_flush(client);

        enet_uint32 wait{ ENET_SOCKET_WAIT_RECEIVE };
        ASSERT_EQ(enet_socket_wait(server, &wait, 2000), 0);
        ASSERT_TRUE(wait & ENET_SOCKET_WAIT_RECEIVE) << "no datagram received";

        std::vector<std::uint8_t> datagram(2048);
        ENetBuffer buffer{};
        buffer.data = datagram.data();
        buffer.dataLength = datagram.size();
        ENetAddress from{};
        const int length{ enet_socket_receive(server, &from, &buffer, 1) };
        ASSERT_GT(length, 0);
        datagram.resize(static_cast<std::size_t>(length));
        std::printf("CONNECT datagram (%d bytes): %s\n", length, hex(datagram).c_str());

        // integrity[3], peerID, [sentTime], checksum
        ASSERT_GE(datagram.size(), 8u + 4u);
        const std::uint16_t integrity0{ be16(&datagram[0]) };
        const std::uint16_t integrity1{ be16(&datagram[2]) };
        const std::uint16_t integrity2{ be16(&datagram[4]) };
        const std::uint16_t peer_field{ be16(&datagram[6]) };

        EXPECT_EQ(static_cast<std::uint16_t>(integrity0 ^ integrity1), server_address.port)
            << "integrity[0] ^ integrity[1] must equal the destination port";
        EXPECT_LE(integrity0, server_address.port);
        EXPECT_EQ(integrity2 & 0x920D, 0x920D);
        EXPECT_EQ(integrity2 & ~(0x61D2 | 0x920D) & 0xFFFF, 0);
        EXPECT_EQ(peer_field & ENET_PROTOCOL_MAXIMUM_PEER_ID, ENET_PROTOCOL_MAXIMUM_PEER_ID) << "CONNECT uses peer id 0xFFF";

        const bool has_sent_time{ (peer_field & ENET_PROTOCOL_HEADER_FLAG_SENT_TIME) != 0 };
        const bool compressed{ (peer_field & ENET_PROTOCOL_HEADER_FLAG_COMPRESSED) != 0 };
        const std::size_t header_size{ has_sent_time ? 10u : 8u };
        ASSERT_GE(datagram.size(), header_size + 4u);

        std::uint32_t checksum{};
        std::memcpy(&checksum, &datagram[header_size], sizeof(checksum));

        // Recompute: header with checksum field = 0 (no connect ID yet for
        // peer 0xFFF) followed by the uncompressed commands.
        std::vector<std::uint8_t> header(datagram.begin(), datagram.begin() + static_cast<std::ptrdiff_t>(header_size + 4));
        std::memset(&header[header_size], 0, 4);
        std::vector<std::uint8_t> payload(datagram.begin() + static_cast<std::ptrdiff_t>(header_size + 4), datagram.end());
        if (compressed) {
            std::vector<std::uint8_t> out(4096);
            const auto size{ enet_range_coder_decompress(
                client->compressor.context, payload.data(), payload.size(), out.data(), out.size()
            ) };
            ASSERT_GT(size, 0u) << "compressed payload does not decompress";
            out.resize(size);
            payload = out;
        }
        ENetBuffer parts[2]{};
        parts[0].data = header.data();
        parts[0].dataLength = header.size();
        parts[1].data = payload.data();
        parts[1].dataLength = payload.size();
        EXPECT_EQ(enet_crc32(parts, 2), checksum) << "checksum mismatch";

        // First command is CONNECT (command 2) with the acknowledge flag.
        ASSERT_GE(payload.size(), 4u);
        EXPECT_EQ(payload[0] & 0x0F, 2) << "first command must be CONNECT";
        EXPECT_NE(payload[0] & 0x80, 0) << "CONNECT must request an acknowledgement";

        enet_host_destroy(client);
    }

    enet_socket_destroy(server);
}
