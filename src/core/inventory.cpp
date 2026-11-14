#include "inventory.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>

#include <fmt/format.h>

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

std::vector<std::string_view> split(const std::string_view text, const char separator)
{
    std::vector<std::string_view> parts;
    std::size_t begin{ 0 };
    while (true) {
        const auto end{ text.find(separator, begin) };
        parts.push_back(text.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin));
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return parts;
}

template <typename T>
std::optional<T> number(const std::string_view text)
{
    T value{};
    const auto [ptr, ec]{ std::from_chars(text.data(), text.data() + text.size(), value) };
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::string lower(const std::string_view text)
{
    std::string out{ text };
    std::ranges::transform(out, out.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// "How many to destroy? (you have 200)" -> 200
std::optional<std::uint32_t> owned_amount(const std::string_view text)
{
    const std::string low{ lower(text) };
    const auto at{ low.find("you have ") };
    if (at == std::string::npos) {
        return std::nullopt;
    }

    std::size_t begin{ at + 9 };
    std::size_t end{ begin };
    while (end < low.size() && std::isdigit(static_cast<unsigned char>(low[end]))) {
        ++end;
    }
    return number<std::uint32_t>(std::string_view{ low }.substr(begin, end - begin));
}

std::string strip_colors(const std::string_view text)
{
    std::string out;
    for (std::size_t i{ 0 }; i < text.size(); ++i) {
        if (text[i] == '`') {
            ++i;
            continue;
        }
        out += text[i];
    }
    return out;
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

std::optional<ItemDialog> parse_item_dialog(const std::string_view markup)
{
    ItemDialog dialog{};
    std::optional<std::uint32_t> prefilled;
    std::optional<std::uint32_t> owned;
    bool has_item{ false };

    for (const auto line : split(markup, '\n')) {
        const auto parts{ split(line, '|') };
        if (parts.empty()) {
            continue;
        }

        const auto key{ parts[0] };
        if (key == "end_dialog" && parts.size() > 1) {
            dialog.dialog_name = std::string{ parts[1] };
        } else if (key == "embed_data" && parts.size() > 2 && parts[1] == "itemID") {
            if (const auto id{ number<std::uint16_t>(parts[2]) }) {
                dialog.item_id = *id;
                has_item = true;
            }
        } else if (key == "add_text_input" && parts.size() > 3 && parts[1] == "count") {
            if (const auto value{ number<std::uint32_t>(parts[3]) }; value && *value > 0) {
                prefilled = *value;
            }
        } else if ((key == "add_textbox" || key == "add_smalltext") && parts.size() > 1) {
            if (const auto value{ owned_amount(parts[1]) }) {
                owned = *value;
            }
        } else if (key == "add_label_with_icon" && parts.size() > 2 && dialog.item_name.empty()) {
            std::string title{ strip_colors(parts[2]) };
            for (const std::string_view prefix : { "Drop ", "Trash ", "Recycle " }) {
                if (title.starts_with(prefix)) {
                    title.erase(0, prefix.size());
                    break;
                }
            }
            dialog.item_name = std::move(title);
        }
    }

    if (dialog.dialog_name != "drop_item" && dialog.dialog_name != "trash_item") {
        return std::nullopt;
    }
    if (!has_item) {
        return std::nullopt;
    }

    dialog.count = prefilled.value_or(owned.value_or(0));
    if (dialog.dialog_name == "trash_item" && owned) {
        dialog.count = *owned;
    }
    return dialog;
}

std::string item_dialog_reply(const std::string_view dialog_name, const std::uint16_t item_id, const std::uint32_t count)
{
    return fmt::format("action|dialog_return\ndialog_name|{}\nitemID|{}|\ncount|{}\n", dialog_name, item_id, count);
}

std::string drop_request(const std::uint16_t item_id)
{
    return fmt::format("action|drop\n|itemID|{}\n", item_id);
}

std::string trash_request(const std::uint16_t item_id)
{
    return fmt::format("action|trash\n|itemID|{}\n", item_id);
}

std::uint64_t lock_value(const std::string_view item_name)
{
    if (item_name == "World Lock") {
        return 1;
    }
    if (item_name == "Diamond Lock") {
        return 100;
    }
    if (item_name == "Blue Gem Lock") {
        return 10'000;
    }
    if (item_name == "Black Gem Lock") {
        return 1'000'000;
    }
    return 0;
}
}
