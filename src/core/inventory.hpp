#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// The inventory packet, kept free of network code so it can be unit tested.
// What is done with the inventory (dropping, locks ...) is in modules/drop.
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
}
