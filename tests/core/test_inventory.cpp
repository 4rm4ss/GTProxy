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

TEST(InventoryTest, DropDialog)
{
    const std::string markup{
        "set_default_color|`o\n"
        "add_label_with_icon|big|`wDrop Diamond Lock``|left|1796|\n"
        "add_textbox|How many to drop?|left|\n"
        "add_text_input|count||7|5|\n"
        "embed_data|itemID|1796\n"
        "end_dialog|drop_item|Cancel|OK|\n"
    };
    const auto dialog{ parse_item_dialog(markup) };
    ASSERT_TRUE(dialog);
    EXPECT_EQ(dialog->dialog_name, "drop_item");
    EXPECT_EQ(dialog->item_id, 1796);
    EXPECT_EQ(dialog->count, 7u);
    EXPECT_EQ(dialog->item_name, "Diamond Lock");
}

TEST(InventoryTest, TrashDialogUsesOwnedAmount)
{
    const std::string markup{
        "add_label_with_icon|big|`4Trash`` `wDirt``|left|2|\n"
        "add_textbox|How many to `4destroy``? (you have 143)|left|\n"
        "add_text_input|count||0|5|\n"
        "embed_data|itemID|2\n"
        "end_dialog|trash_item|Cancel|OK|\n"
    };
    const auto dialog{ parse_item_dialog(markup) };
    ASSERT_TRUE(dialog);
    EXPECT_EQ(dialog->dialog_name, "trash_item");
    EXPECT_EQ(dialog->item_id, 2);
    EXPECT_EQ(dialog->count, 143u);
}

TEST(InventoryTest, OtherDialogsAreNotItemDialogs)
{
    EXPECT_FALSE(parse_item_dialog("add_button|x|X|noflags|0|0|\nend_dialog|popup|Close||\n"));
    EXPECT_FALSE(parse_item_dialog("end_dialog|drop_item|Cancel|OK|\n")); // no item id
    EXPECT_FALSE(parse_item_dialog(""));

    // Another dialog about an item is read, with its own name.
    const auto info{ parse_item_dialog("embed_data|itemID|242\nend_dialog|item_info|Close||\n") };
    ASSERT_TRUE(info);
    EXPECT_EQ(info->dialog_name, "item_info");
}

TEST(InventoryTest, RequestsAndReplies)
{
    EXPECT_EQ(drop_request(242), "action|drop\n|itemID|242\n");
    EXPECT_EQ(trash_request(2), "action|trash\n|itemID|2\n");
    EXPECT_EQ(item_dialog_reply("drop_item", 242, 5), "action|dialog_return\ndialog_name|drop_item\nitemID|242|\ncount|5\n");
}

TEST(InventoryTest, PickLocksBiggestFirst)
{
    const std::vector<LockStack> have{ { 242, 1, 150 }, { 1796, 100, 5 }, { 7188, 10'000, 1 } };

    const auto three_dl{ pick_locks(have, 300) };
    ASSERT_TRUE(three_dl);
    EXPECT_EQ(*three_dl, (std::vector<std::pair<std::uint16_t, std::uint32_t>>{ { 1796, 3 } }));

    const auto mixed{ pick_locks(have, 10'250) };
    ASSERT_TRUE(mixed);
    EXPECT_EQ(*mixed, (std::vector<std::pair<std::uint16_t, std::uint32_t>>{ { 7188, 1 }, { 1796, 2 }, { 242, 50 } }));

    // Not enough DLs: the rest is paid in WLs.
    const auto more_wl{ pick_locks(have, 640) };
    ASSERT_TRUE(more_wl);
    EXPECT_EQ(*more_wl, (std::vector<std::pair<std::uint16_t, std::uint32_t>>{ { 1796, 5 }, { 242, 140 } }));

    EXPECT_EQ(pick_locks(have, 3), (std::vector<std::pair<std::uint16_t, std::uint32_t>>{ { 242, 3 } }));
    EXPECT_FALSE(pick_locks({ { 1796, 100, 5 } }, 250));            // no WLs for the 50
    EXPECT_FALSE(pick_locks(have, 20'000));                          // only one BGL
    EXPECT_FALSE(pick_locks({}, 1));
}

TEST(InventoryTest, LockValues)
{
    // Known IDs work without items.dat (no name).
    EXPECT_EQ(lock_value(242), 1u);
    EXPECT_EQ(lock_value(1796), 100u);
    EXPECT_EQ(lock_value(7188), 10'000u);
    EXPECT_EQ(known_name(242), "World Lock");
    EXPECT_EQ(known_name(2), "");
    // Newer locks by name.
    EXPECT_EQ(lock_value(20000, "Black Gem Lock"), 1'000'000u);
    EXPECT_EQ(lock_value(202, "Small Lock"), 0u);
    EXPECT_EQ(lock_value(243, "World Lock Seed"), 0u);
}
