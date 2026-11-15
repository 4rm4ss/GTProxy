#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "core/inventory.hpp"

using namespace core::inventory;

namespace {
std::vector<std::byte> inventory_bytes(const std::uint32_t slots, const std::vector<Item>& items)
{
    std::vector<std::byte> out;
    const auto put = [&out](const auto value) {
        const auto* p{ reinterpret_cast<const std::byte*>(&value) };
        out.insert(out.end(), p, p + sizeof(value));
    };
    put(std::uint8_t{ 1 });
    put(slots);
    put(static_cast<std::uint16_t>(items.size()));
    for (const auto& item : items) {
        put(item.id);
        put(item.count);
        put(item.flags);
    }
    return out;
}
}

TEST(InventoryTest, ParsesItemsAndFlags)
{
    const auto data{ inventory_bytes(96, { { 18, 1, 0 }, { 2, 200, 0 }, { 242, 37, 0x02 }, { 48, 1, 0x01 } }) };
    const auto inv{ parse(data) };
    ASSERT_TRUE(inv);
    EXPECT_EQ(inv->slots, 96u);
    ASSERT_EQ(inv->items.size(), 4u);
    EXPECT_EQ(inv->items[1].id, 2);
    EXPECT_EQ(inv->items[1].count, 200);
    EXPECT_EQ(inv->items[2].flags, 0x02);
    EXPECT_EQ(inv->items[3].flags, FLAG_WORN);
}

TEST(InventoryTest, RejectsTruncatedData)
{
    auto data{ inventory_bytes(16, { { 2, 5, 0 }, { 4, 6, 0 } }) };
    data.pop_back();
    EXPECT_FALSE(parse(data));
    EXPECT_FALSE(parse({}));
    EXPECT_TRUE(parse(inventory_bytes(16, {})));
}
