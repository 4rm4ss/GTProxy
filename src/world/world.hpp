#pragma once
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include "object_map.hpp"
#include "tile_map.hpp"
#include "../player/player.hpp"
#include "../utils/byte_stream.hpp"
#include "../utils/singleton.hpp"

namespace world {
class World : public utils::Singleton<World> {
public:
    World()
        : local_net_id_{ -1 }
        , version_{ 0 }
    {

    }

    void add_player(const std::shared_ptr<player::Player>& player)
    {
        if (player->is_local()) {
            local_net_id_ = player->net_id();
        }

        players_[player->net_id()] = player;
    }

    void remove_player(const int32_t net_id)
    {
        if (net_id != local_net_id_) {
            players_.erase(net_id);
        }
    }

    void clear()
    {
        players_.clear();
        local_net_id_ = -1;
        tile_map_ = WorldTileMap{};
        object_map_ = WorldObjectMap{};
    }

    [[nodiscard]] std::weak_ptr<player::Player> get_player(const int32_t net_id) const
    {
        if (const auto it{ players_.find(net_id) }; it != players_.end()) {
            return it->second;
        }

        return {};
    }

    [[nodiscard]] std::weak_ptr<player::Player> get_local_player() const
    {
        if (local_net_id_ < 0) {
            return {};
        }

        return get_player(local_net_id_);
    }

    [[nodiscard]] const std::unordered_map<int32_t, std::shared_ptr<player::Player>>& get_players() const
    {
        return players_;
    }

    [[nodiscard]] int32_t get_local_net_id() const
    {
        return local_net_id_;
    }

    [[nodiscard]] WorldTileMap& get_tile_map()
    {
        return tile_map_;
    }

    [[nodiscard]] const WorldTileMap& get_tile_map() const
    {
        return tile_map_;
    }

    [[nodiscard]] WorldObjectMap& get_object_map()
    {
        return object_map_;
    }

    [[nodiscard]] const WorldObjectMap& get_object_map() const
    {
        return object_map_;
    }

    [[nodiscard]] uint16_t get_version() const
    {
        return version_;
    }

    // Name of the world from the last map data (upper case, e.g. "START").
    [[nodiscard]] const std::string& get_name() const
    {
        return name_;
    }

    // Returns false if the map data could not be fully parsed (e.g. the world
    // format changed in a newer game version). Never reads out of bounds.
    bool serialize(const std::byte* extended_data, const std::size_t extended_data_size)
    {
        if (!extended_data) {
            return false;
        }

        utils::ByteStream bs{ extended_data, extended_data_size };

        bs.read(version_);
        bs.skip(4);
        bs.read(name_);

        if (!bs.good() || !tile_map_.serialize(bs, version_)) {
            return false;
        }

        return object_map_.serialize(bs, version_);
    }

    // A tile the server sent again in full (SendTileUpdateData: locks, signs,
    // doors ...). False when it is outside the world or does not parse; the
    // tile is then left as it was.
    bool update_tile(const int x, const int y, const std::byte* data, const std::size_t size)
    {
        const auto index{ tile_index(x, y) };
        if (!index || !data) {
            return false;
        }

        utils::ByteStream bs{ data, size };
        Tile tile{};
        tile.serialize(bs, version_);
        if (!bs.good()) {
            return false;
        }

        tile_map_.get_tiles()[*index] = std::move(tile);
        return true;
    }

    // The server says a tile changed (TileChangeRequest): a punch with the
    // fist broke its foreground (or, without one, its background), or an
    // item was placed on it. Returns whether the tile changed.
    bool change_tile(const int x, const int y, const std::int32_t item_id)
    {
        const auto index{ tile_index(x, y) };
        if (!index) {
            return false;
        }

        auto& tile{ tile_map_.get_tiles()[*index] };
        constexpr std::int32_t FIST{ 18 };
        if (item_id == FIST) {
            if (tile.foreground != 0) {
                // The block and everything attached to it (lock, sign text ...).
                Tile cleared{};
                cleared.background = tile.background;
                tile = std::move(cleared);
                return true;
            }
            if (tile.background != 0) {
                tile.background = 0;
                return true;
            }
            return false;
        }

        const auto* item{ item::ItemDatabase::instance().get_item(static_cast<std::uint32_t>(item_id)) };
        if (!item || item_id <= 0) {
            return false;
        }

        switch (item->item_type) {
        case item::ItemType::Background:
        case item::ItemType::BackgdSfxExtraFrame:
        case item::ItemType::BackBoombox:
            tile.background = static_cast<std::uint16_t>(item_id);
            return true;
        // Used on a tile or on yourself, never placed.
        case item::ItemType::Fist:
        case item::ItemType::Wrench:
        case item::ItemType::Consumable:
        case item::ItemType::Clothes:
        case item::ItemType::Gems:
            return false;
        default:
            tile.foreground = static_cast<std::uint16_t>(item_id);
            return true;
        }
    }

    [[nodiscard]] const Tile* get_tile(const int x, const int y) const
    {
        const auto index{ tile_index(x, y) };
        return index ? &tile_map_.get_tiles()[*index] : nullptr;
    }

private:
    [[nodiscard]] std::optional<std::size_t> tile_index(const int x, const int y) const
    {
        const auto& size{ tile_map_.get_size() };
        if (x < 0 || y < 0 || x >= size.x || y >= size.y) {
            return std::nullopt;
        }

        const auto index{ static_cast<std::size_t>(y) * static_cast<std::size_t>(size.x) + static_cast<std::size_t>(x) };
        if (index >= tile_map_.get_tiles().size()) {
            return std::nullopt;
        }
        return index;
    }

    std::unordered_map<int32_t, std::shared_ptr<player::Player>> players_;
    int32_t local_net_id_;

    uint16_t version_;
    std::string name_;

    WorldTileMap tile_map_;
    WorldObjectMap object_map_;
};
}
