#include <cstdint>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

#include "item/item_database.hpp"
#include "utils/byte_stream.hpp"
#include "world/object_map.hpp"
#include "world/tile_map.hpp"

using utils::ByteStream;

namespace {
std::vector<std::byte> to_vector(const ByteStream<>& bs)
{
    const auto* begin{ bs.get_raw_ptr() };
    return { begin, begin + bs.get_size() };
}

void write_tile_map_header(ByteStream<>& bs, const std::int32_t width, const std::int32_t height, const std::uint32_t count)
{
    bs.write<std::int32_t>(width);
    bs.write<std::int32_t>(height);
    bs.write<std::uint32_t>(count);
    for (int i = 0; i < 5; ++i) {
        bs.write<std::uint8_t>(0);
    }
}

void write_plain_tile(ByteStream<>& bs)
{
    bs.write<std::uint16_t>(0);     // foreground
    bs.write<std::uint16_t>(0);     // background
    bs.write<std::uint16_t>(0);     // parent tile
    bs.write<std::uint16_t>(0);     // flags
}
}

class WorldParsingTest : public ::testing::Test {
protected:
    // Item lookups must be safe even when no items.dat has been loaded.
    void SetUp() override { item::ItemDatabase::instance().clear(); }
};

TEST_F(WorldParsingTest, ParsesValidTileMap)
{
    ByteStream<> writer{};
    write_tile_map_header(writer, 2, 1, 2);
    write_plain_tile(writer);
    write_plain_tile(writer);
    const auto data{ to_vector(writer) };

    ByteStream<> bs{ data };
    WorldTileMap map{};
    EXPECT_TRUE(map.serialize(bs, 0));
    EXPECT_EQ(map.get_tiles().size(), 2u);
    EXPECT_EQ(map.get_size().x, 2);
}

TEST_F(WorldParsingTest, RejectsHugeTileCountWithoutAllocating)
{
    ByteStream<> writer{};
    write_tile_map_header(writer, 100, 60, 0xFFFFFFFFu);
    write_plain_tile(writer);
    const auto data{ to_vector(writer) };

    ByteStream<> bs{ data };
    WorldTileMap map{};
    EXPECT_FALSE(map.serialize(bs, 0));
    EXPECT_TRUE(map.get_tiles().empty());
}

TEST_F(WorldParsingTest, FailsOnTruncatedTiles)
{
    ByteStream<> writer{};
    write_tile_map_header(writer, 2, 1, 2);
    write_plain_tile(writer);
    writer.write<std::uint16_t>(0); // half of the second tile
    const auto data{ to_vector(writer) };

    ByteStream<> bs{ data };
    WorldTileMap map{};
    EXPECT_FALSE(map.serialize(bs, 0));
}

TEST_F(WorldParsingTest, RejectsHugeObjectCount)
{
    ByteStream<> writer{};
    for (int i = 0; i < 12; ++i) {
        writer.write<std::uint8_t>(0);
    }
    writer.write<std::uint32_t>(0x7FFFFFFFu);   // object count
    writer.write<std::uint32_t>(1);             // drop id
    const auto data{ to_vector(writer) };

    ByteStream<> bs{ data };
    WorldObjectMap map{};
    EXPECT_FALSE(map.serialize(bs, 0));
    EXPECT_TRUE(map.get_objects().empty());
}

TEST_F(WorldParsingTest, GarbageInputNeverCrashes)
{
    // Pseudo-random garbage of different sizes must be rejected or parsed,
    // but never read out of bounds or allocate unbounded memory.
    std::uint32_t seed{ 12345 };
    for (std::size_t size = 0; size < 512; size += 7) {
        std::vector<std::byte> data(size);
        for (auto& b : data) {
            seed = seed * 1103515245u + 12345u;
            b = static_cast<std::byte>(seed >> 16);
        }

        ByteStream<> bs{ data };
        WorldTileMap tiles{};
        WorldObjectMap objects{};
        if (tiles.serialize(bs, 0)) {
            std::ignore = objects.serialize(bs, 0);
        }

        EXPECT_LE(bs.get_read_offset(), data.size());
    }
}
