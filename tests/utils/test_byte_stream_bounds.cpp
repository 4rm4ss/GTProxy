#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "utils/byte_stream.hpp"

using utils::ByteStream;

namespace {
std::vector<std::byte> make_bytes(const std::size_t size)
{
    return std::vector<std::byte>(size, std::byte{ 0xAB });
}
}

TEST(ByteStreamBoundsTest, SkipPastEndFailsAndClamps)
{
    const auto data{ make_bytes(4) };
    ByteStream<> bs{ data };

    EXPECT_TRUE(bs.good());
    EXPECT_FALSE(bs.skip(10));
    EXPECT_FALSE(bs.good());
    EXPECT_EQ(bs.get_read_offset(), data.size());
    EXPECT_EQ(bs.get_remaining(), 0u);

    // Reads after an overrun must fail instead of reading out of bounds.
    std::uint32_t value{};
    EXPECT_FALSE(bs.read(value));
    std::string str;
    EXPECT_FALSE(bs.read(str, 2));
}

TEST(ByteStreamBoundsTest, BacktrackBeforeStartFails)
{
    const auto data{ make_bytes(4) };
    ByteStream<> bs{ data };

    EXPECT_TRUE(bs.skip(2));
    EXPECT_FALSE(bs.backtrack(3));
    EXPECT_FALSE(bs.good());
    EXPECT_EQ(bs.get_read_offset(), 0u);
}

TEST(ByteStreamBoundsTest, StringLengthLargerThanDataFails)
{
    ByteStream<> writer{};
    writer.write<std::uint16_t>(500);
    writer.write<std::uint8_t>(1);

    ByteStream<> bs{ writer.get_raw_ptr(), writer.get_size() };
    std::string str;
    EXPECT_FALSE(bs.read(str));
    EXPECT_FALSE(bs.good());
}

TEST(ByteStreamBoundsTest, CanReadRejectsHugeCounts)
{
    const auto data{ make_bytes(16) };
    ByteStream<> bs{ data };

    EXPECT_TRUE(bs.can_read(4, sizeof(std::uint32_t)));
    EXPECT_TRUE(bs.good());
    EXPECT_FALSE(bs.can_read(0xFFFFFFFFu, sizeof(std::uint32_t)));
    EXPECT_FALSE(bs.good());
}

TEST(ByteStreamBoundsTest, ExactReadsStayGood)
{
    ByteStream<> writer{};
    writer.write<std::uint32_t>(7);
    writer.write("abc");

    ByteStream<> bs{ writer.get_raw_ptr(), writer.get_size() };
    std::uint32_t value{};
    std::string str;
    EXPECT_TRUE(bs.read(value));
    EXPECT_TRUE(bs.read(str));
    EXPECT_EQ(value, 7u);
    EXPECT_EQ(str, "abc");
    EXPECT_EQ(bs.get_remaining(), 0u);
    EXPECT_TRUE(bs.good());
}
