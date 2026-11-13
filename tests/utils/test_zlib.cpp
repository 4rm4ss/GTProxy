#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "utils/zlib.hpp"

namespace {
std::vector<std::byte> make_payload(const std::size_t size)
{
    std::vector<std::byte> data(size);
    for (std::size_t i = 0; i < size; ++i) {
        data[i] = static_cast<std::byte>((i * 31u + i / 7u) & 0xFFu);
    }
    return data;
}
}

TEST(ZlibTest, RoundTripWithoutExpectedSize)
{
    const auto original{ make_payload(100000) };
    std::vector<std::byte> compressed;
    ASSERT_TRUE(utils::compress_zlib(original, compressed));

    std::vector<std::byte> decompressed;
    ASSERT_TRUE(utils::decompress_zlib(compressed, decompressed));
    EXPECT_EQ(decompressed, original);
}

// This is how SendItemDatabaseData decompresses items.dat sent by the server.
TEST(ZlibTest, RoundTripWithExpectedSize)
{
    const auto original{ make_payload(100000) };
    std::vector<std::byte> compressed;
    ASSERT_TRUE(utils::compress_zlib(original, compressed));

    std::vector<std::byte> decompressed;
    ASSERT_TRUE(utils::decompress_zlib(compressed, decompressed, static_cast<std::uint32_t>(original.size())));
    EXPECT_EQ(decompressed, original);
}

TEST(ZlibTest, RejectsWrongExpectedSize)
{
    const auto original{ make_payload(5000) };
    std::vector<std::byte> compressed;
    ASSERT_TRUE(utils::compress_zlib(original, compressed));

    std::vector<std::byte> decompressed;
    EXPECT_FALSE(utils::decompress_zlib(compressed, decompressed, 100));
    EXPECT_FALSE(utils::decompress_zlib(compressed, decompressed, 6000));
}

TEST(ZlibTest, HugeExpectedSizeDoesNotPreallocate)
{
    const auto original{ make_payload(1000) };
    std::vector<std::byte> compressed;
    ASSERT_TRUE(utils::compress_zlib(original, compressed));

    // A bogus size from the packet header must not trigger a 4 GiB allocation.
    std::vector<std::byte> decompressed;
    EXPECT_FALSE(utils::decompress_zlib(compressed, decompressed, 0xFFFFFFF0u));
}

TEST(ZlibTest, RejectsGarbage)
{
    const auto garbage{ make_payload(256) };
    std::vector<std::byte> decompressed;
    EXPECT_FALSE(utils::decompress_zlib(garbage, decompressed, 1024));
}
