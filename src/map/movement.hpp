#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "coords.hpp"
#include "pathfinder.hpp"
#include "../packet/packet_types.hpp"

namespace map {
// Moves a point along a polyline at a constant speed (world pixels per
// second), the way an avatar runs from tile to tile.
class Walker {
public:
    struct Step {
        glm::vec2 position;
        glm::vec2 velocity;     // zero once the end is reached
    };

    Walker() = default;
    // points[0] is where you start; speed must be > 0.
    Walker(std::vector<glm::vec2> points, float speed);

    // Moves on by 'seconds' and returns where that is.
    Step advance(float seconds);

    [[nodiscard]] bool finished() const { return next_ >= points_.size(); }
    [[nodiscard]] glm::vec2 position() const { return position_; }
    [[nodiscard]] const std::vector<glm::vec2>& points() const { return points_; }
    // Index of the point being walked to (points().size() when finished).
    [[nodiscard]] std::size_t next_point() const { return next_; }

private:
    std::vector<glm::vec2> points_;
    std::size_t next_{ 0 };
    glm::vec2 position_{ 0.0f };
    float speed_{ 0.0f };
};

// Avatar positions for a path: the top-left corner of each tile, as the game
// places a player standing in it.
[[nodiscard]] std::vector<glm::vec2> path_to_points(const Path& path);

struct MoveState {
    glm::vec2 position{ 0.0f };     // world pixels
    glm::vec2 velocity{ 0.0f };     // world pixels per second
    bool facing_left{ false };
    bool on_ground{ false };
};

// The state packet the game sends to the server while you move. 'model' is
// the last one the game itself sent (fields this proxy does not understand
// are kept from it); position, speed, facing, "on solid" and the punch target
// come from 'state'.
[[nodiscard]] packet::GameUpdatePacket make_state_packet(
    const packet::GameUpdatePacket& model,
    std::int32_t net_id,
    const MoveState& state
);
}
