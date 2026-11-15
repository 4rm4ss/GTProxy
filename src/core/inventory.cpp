#include "inventory.hpp"

#include <cstring>

namespace core::inventory {
namespace {
template <typename T>
bool read(const std::span<const std::byte> data, std::size_t& offset, T& out)
{
    if (offset + sizeof(T) > data.size()) {
        return false;
    }
    std::memcpy(&out, data.data() + offset, sizeof(T));
    offset += sizeof(T);
    return true;
}
}

std::optional<Inventory> parse(const std::span<const std::byte> data)
{
    std::size_t offset{ 0 };
    std::uint8_t unknown{};
    Inventory inventory{};
    std::uint16_t count{};
    if (!read(data, offset, unknown) || !read(data, offset, inventory.slots) || !read(data, offset, count)) {
        return std::nullopt;
    }

    if (offset + static_cast<std::size_t>(count) * 4 > data.size()) {
        return std::nullopt;
    }

    inventory.items.reserve(count);
    for (std::uint16_t i{ 0 }; i < count; ++i) {
        Item item{};
        read(data, offset, item.id);
        read(data, offset, item.count);
        read(data, offset, item.flags);
        inventory.items.push_back(item);
    }
    return inventory;
}
}
