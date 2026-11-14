#pragma once
#include <vector>
#include <glm/glm.hpp>

#include "tile.hpp"
#include "../utils/byte_stream.hpp"

class WorldTileMap final {
public:
    WorldTileMap()
        : size_{ 0, 0 }
    {

    }

    bool serialize(utils::ByteStream<>& bs, const std::uint16_t version)
    {
        // foreground, background, parent tile and flags (u16 each)
        constexpr std::size_t MIN_TILE_SIZE{ 8 };

        bs.read(size_.x);
        bs.read(size_.y);

        std::uint32_t count{};
        bs.read(count);
        bs.skip(5);

        if (!bs.good() || !bs.can_read(count, MIN_TILE_SIZE)) {
            tiles_.clear();
            return false;
        }

        tiles_.resize(count);

        for (auto& tile : tiles_) {
            tile.serialize(bs, version);
            if (!bs.good()) {
                return false;
            }
        }

        return true;
    }

    [[nodiscard]] const glm::ivec2& get_size() const { return size_; }
    [[nodiscard]] glm::ivec2& get_size() { return size_; }
    [[nodiscard]] const std::vector<world::Tile>& get_tiles() const { return tiles_; }
    [[nodiscard]] std::vector<world::Tile>& get_tiles() { return tiles_; }

private:
    glm::ivec2 size_;
    std::vector<world::Tile> tiles_;
};
