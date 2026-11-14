#include "item_database.hpp"

#include <algorithm>
#include <cctype>

#include <spdlog/spdlog.h>
#include <fstream>

namespace item {
namespace {
const std::string_view DECRYPT_KEY = "PBG892FXX982ABC*";
}

std::string ItemDatabase::decrypt_item_name(std::string_view encrypted, std::uint32_t item_id)
{
    std::string decrypted;
    decrypted.reserve(encrypted.size());

    for (std::size_t i = 0; i < encrypted.size(); ++i) {
        const std::size_t key_index = (i + item_id) % DECRYPT_KEY.size();
        decrypted += static_cast<char>(encrypted[i] ^ DECRYPT_KEY[key_index]);
    }

    return decrypted;
}

std::optional<ItemInfo> ItemDatabase::parse_item(utils::ByteStream<>& bs) const
{
    ItemInfo item{};
    bool ok{ true };
    const auto read_field{ [&bs, &ok](auto& value) { ok = bs.read(value) && ok; } };

    read_field(item.item_id);
    read_field(item.flags);
    read_field(item.item_type);
    read_field(item.material);

    read_field(item.item_name);
    item.item_name = decrypt_item_name(item.item_name, item.item_id);

    read_field(item.texture_file_path);
    read_field(item.texture_file_hash);

    read_field(item.visual_effect);
    read_field(item.cooking_time);

    read_field(item.texture_coord_x);
    read_field(item.texture_coord_y);

    read_field(item.texture_type);

    read_field(item.is_stripey_wallpaper);

    read_field(item.collision_type);

    read_field(item.health);
    read_field(item.reset_time);

    read_field(item.clothing_type);

    read_field(item.rarity);
    read_field(item.max_amount);

    read_field(item.extra_file);
    read_field(item.extra_file_hash);

    read_field(item.animation_time);

    read_field(item.pet_name);
    read_field(item.pet_prefix);
    read_field(item.pet_suffix);
    read_field(item.pet_ability);

    read_field(item.seed_base);
    read_field(item.seed_overlay);
    read_field(item.tree_base);
    read_field(item.tree_leaves);
    read_field(item.seed_color);
    read_field(item.seed_overlay_color);
    read_field(item.ingredient);
    read_field(item.grow_time);

    read_field(item.fx_flag);

    read_field(item.animating_coordinates);
    read_field(item.animating_texture_file);
    read_field(item.animating_coordinates_2);

    read_field(item.__unk_1);
    read_field(item.__unk_2);

    read_field(item.flags_2);

    read_field(item.__unk_3);

    read_field(item.tile_range);

    read_field(item.vault_capacity);

    read_field(item.punch_options);

    read_field(item.__unk_4);

    read_field(item.body_part_list);

    read_field(item.light_range);

    read_field(item.__unk_5);

    read_field(item.can_sit);
    read_field(item.player_offset_x);
    read_field(item.player_offset_y);
    read_field(item.chair_texture_x);
    read_field(item.chair_texture_y);
    read_field(item.chair_leg_offset_x);
    read_field(item.chair_leg_offset_y);
    read_field(item.chair_texture_file);
    read_field(item.renderer_data_file);

    read_field(item.__unk_6);
    read_field(item.renderer_data_file_hash);

    read_field(item.__unk_7);
    read_field(item.__unk_8);

    read_field(item.info);

    read_field(item.ingredients);

    if (version_ >= 24) {
        read_field(item.__unk_9);
    }

    if (version_ >= 25) {
        read_field(item.hit_sound_file);
        read_field(item.hit_sound_file_hash);
    }

    if (version_ >= 26) {
        read_field(item.__unk_10);
    }

    if (version_ >= 27) {
        read_field(item.__unk_11);
    }

    if (!ok) {
        return std::nullopt;
    }

    return item;
}

bool ItemDatabase::load_from_file(const std::string& file_path)
{
    std::ifstream file{ file_path, std::ios::binary | std::ios::ate };
    if (!file) {
        spdlog::error("Failed to open items.dat: {}", file_path);
        return false;
    }

    const auto file_size{ file.tellg() };
    file.seekg(0, std::ios::beg);

    std::vector<std::byte> buffer(file_size);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), file_size)) {
        spdlog::error("Failed to read items.dat: {}", file_path);
        return false;
    }

    return parse(buffer);
}

bool ItemDatabase::parse(std::span<const std::byte> data)
{
    clear();

    utils::ByteStream<> bs{ data };

    if (!bs.read(version_) || !bs.read(count_)) {
        spdlog::error("Failed to read items.dat header");
        return false;
    }

    spdlog::info("Parsing items.dat: version={}, count={}", version_, count_);

    if (version_ > MAX_SUPPORTED_VERSION) {
        spdlog::warn("Unsupported items.dat version: {}, max supported is {}", version_, MAX_SUPPORTED_VERSION);
        // Don't fail, just warn - parsing might still work for newer versions
    }

    // Every item has well over 64 bytes of fixed-size fields; reject counts the
    // data cannot possibly hold before reserving memory for them.
    constexpr std::size_t MIN_ITEM_SIZE{ 64 };
    if (!bs.can_read(count_, MIN_ITEM_SIZE)) {
        spdlog::error("items.dat item count {} does not fit in {} bytes", count_, data.size());
        clear();
        return false;
    }

    items_.reserve(count_);

    for (std::uint32_t i = 0; i < count_; ++i) {
        auto item_result{ parse_item(bs) };
        if (!item_result.has_value()) {
            spdlog::error("Failed to parse item at index {} (items.dat version {}, count {})", i, version_, count_);
            clear();
            return false;
        }

        auto& item{ item_result.value() };
        if (item.item_id != i) {
            spdlog::warn("Item ID mismatch at index {}: expected {}, got {}", i, i, item.item_id);
        }

        items_.push_back(std::move(item));
    }

    if (bs.get_read_offset() != bs.get_size()) {
        spdlog::warn(
            "items.dat has {} trailing bytes after parsing (version {}), format may have changed",
            bs.get_size() - bs.get_read_offset(),
            version_
        );
    }

    spdlog::info("Successfully parsed {} items from items.dat", items_.size());
    return true;
}

const ItemInfo* ItemDatabase::get_item(std::uint32_t id) const
{
    if (id >= items_.size()) {
        return nullptr;
    }

    return &items_[id];
}

std::vector<const ItemInfo*> ItemDatabase::find(const std::string_view query, const std::size_t limit) const
{
    const auto lower = [](const std::string_view text) {
        std::string out{ text };
        std::ranges::transform(out, out.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    };

    std::vector<const ItemInfo*> exact;
    std::vector<const ItemInfo*> partial;
    const std::string needle{ lower(query) };
    if (needle.empty() || limit == 0) {
        return {};
    }

    for (const auto& item : items_) {
        const std::string name{ lower(item.item_name) };
        if (name == needle) {
            exact.push_back(&item);
        } else if (name.find(needle) != std::string::npos) {
            partial.push_back(&item);
        }
    }

    exact.insert(exact.end(), partial.begin(), partial.end());
    if (exact.size() > limit) {
        exact.resize(limit);
    }
    return exact;
}

void ItemDatabase::clear()
{
    version_ = 0;
    count_ = 0;
    items_.clear();
}
}
