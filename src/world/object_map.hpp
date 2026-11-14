#pragma once
#include <vector>

#include "object.hpp"
#include "../utils/byte_stream.hpp"

class WorldObjectMap final {
public:
    WorldObjectMap()
        : drop_id_{ 0 }
    {

    }

    bool serialize(utils::ByteStream<>& bs, const std::uint16_t version)
    {
        // item id (u16), position (2x float), amount (u8), flags (u8), object id (u32)
        constexpr std::size_t MIN_OBJECT_SIZE{ 16 };

        bs.skip(12);

        std::uint32_t count{};
        bs.read(count);
        bs.read(drop_id_);

        if (!bs.good() || !bs.can_read(count, MIN_OBJECT_SIZE)) {
            objects_.clear();
            return false;
        }

        objects_.resize(count);
        for (auto& object : objects_) {
            object.serialize(bs, version);
        }

        return bs.good();
    }

    [[nodiscard]] std::uint32_t get_drop_id() const { return drop_id_; }
    void set_drop_id(std::uint32_t id) { drop_id_ = id; }
    void increment_drop_id() { ++drop_id_; }
    [[nodiscard]] const std::vector<world::Object>& get_objects() const { return objects_; }
    [[nodiscard]] std::vector<world::Object>& get_objects() { return objects_; }

private:
    std::uint32_t drop_id_;
    std::vector<world::Object> objects_;
};
