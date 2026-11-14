#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Inventory data and the game's item dialogs (drop / trash), kept free of
// network code so they can be unit tested.
namespace core::inventory {
// Item flag bits in the inventory packet.
inline constexpr std::uint8_t FLAG_WORN{ 0x01 };

struct Item {
    std::uint16_t id{ 0 };
    std::uint8_t count{ 0 };
    std::uint8_t flags{ 0 };
};

struct Inventory {
    std::uint32_t slots{ 0 };
    std::vector<Item> items;
};

// PACKET_SEND_INVENTORY_STATE extra data:
// u8 ?, u32 slots, u16 count, count x (u16 id, u8 amount, u8 flags).
[[nodiscard]] std::optional<Inventory> parse(std::span<const std::byte> data);

// A drop_item / trash_item dialog the server opens after "action|drop" /
// "action|trash".
struct ItemDialog {
    std::string dialog_name;    // "drop_item" or "trash_item"
    std::uint16_t item_id{ 0 };
    std::uint32_t count{ 0 };   // amount the dialog offers (what you own)
    std::string item_name;
};

[[nodiscard]] std::optional<ItemDialog> parse_item_dialog(std::string_view markup);

// Text the game sends to confirm such a dialog with the given amount.
[[nodiscard]] std::string item_dialog_reply(std::string_view dialog_name, std::uint16_t item_id, std::uint32_t count);

// Text the game sends when you press Drop / Trash on an item.
[[nodiscard]] std::string drop_request(std::uint16_t item_id);
[[nodiscard]] std::string trash_request(std::uint16_t item_id);

// World Lock currency: value in World Locks for the lock items, 0 otherwise.
[[nodiscard]] std::uint64_t lock_value(std::string_view item_name);
}
