#include "movement.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace map {
namespace {
// Flags that describe a moment of movement or an event (jumping, being hit,
// punching ...). They are cleared on packets this proxy builds; everything
// else is kept from the game's own last packet.
constexpr std::uint32_t TRANSIENT_FLAGS{
    packet::PACKET_FLAG_RESET_VISUAL_STATE
    | packet::PACKET_FLAG_EXTENDED
    | packet::PACKET_FLAG_ROTATE_LEFT
    | packet::PACKET_FLAG_ON_SOLID
    | packet::PACKET_FLAG_ON_FIRE_DAMAGE
    | packet::PACKET_FLAG_ON_JUMP
    | packet::PACKET_FLAG_ON_KILLED
    | packet::PACKET_FLAG_ON_PUNCHED
    | packet::PACKET_FLAG_ON_PLACED
    | packet::PACKET_FLAG_ON_TILE_ACTION
    | packet::PACKET_FLAG_ON_GOT_PUNCHED
    | packet::PACKET_FLAG_ON_RESPAWNED
    | packet::PACKET_FLAG_ON_COLLECT_OBJECT
    | packet::PACKET_FLAG_ON_TRAMPOLINE
    | packet::PACKET_FLAG_ON_DAMAGE
    | packet::PACKET_FLAG_ON_SLIDE
    | packet::PACKET_FLAG_ON_WALL_HANG
    | packet::PACKET_FLAG_ON_ACID_DAMAGE
};
}

Walker::Walker(std::vector<glm::vec2> points, const float speed)
    : points_{ std::move(points) }
    , next_{ 1 }
    , speed_{ speed }
{
    if (points_.empty() || !(speed_ > 0.0f)) {
        points_.clear();
        next_ = 0;
        return;
    }

    position_ = points_.front();
}

Walker::Step Walker::advance(const float seconds)
{
    float budget{ std::max(seconds, 0.0f) * speed_ };
    glm::vec2 direction{ 0.0f };

    while (!finished()) {
        const glm::vec2 to_next{ points_[next_] - position_ };
        const float distance{ glm::length(to_next) };
        if (distance > 0.0f) {
            direction = to_next / distance;
        }

        if (distance > budget) {
            position_ += direction * budget;
            return { position_, direction * speed_ };
        }

        position_ = points_[next_];
        budget -= distance;
        ++next_;
    }

    return { position_, glm::vec2{ 0.0f } };
}

std::vector<glm::vec2> path_to_points(const Path& path)
{
    std::vector<glm::vec2> points;
    points.reserve(path.size());
    for (const TilePos tile : path) {
        points.push_back(tile_to_world(tile));
    }
    return points;
}

packet::GameUpdatePacket make_state_packet(
    const packet::GameUpdatePacket& model,
    const std::int32_t net_id,
    const MoveState& state
)
{
    packet::GameUpdatePacket pkt{ model };
    pkt.type = packet::PACKET_STATE;
    pkt.net_id = static_cast<std::uint32_t>(net_id);

    std::uint32_t flags{ static_cast<std::uint32_t>(model.flags.value) & ~TRANSIENT_FLAGS };
    if (state.facing_left) {
        flags |= packet::PACKET_FLAG_ROTATE_LEFT;
    }
    if (state.on_ground) {
        flags |= packet::PACKET_FLAG_ON_SOLID;
    }
    pkt.flags.value = static_cast<packet::PacketFlag>(flags);

    pkt.pos_x = state.position.x;
    pkt.pos_y = state.position.y;
    pkt.pos_x2 = state.velocity.x;
    pkt.pos_y2 = state.velocity.y;

    // Not punching or building anything.
    pkt.int_x = -1;
    pkt.int_y = -1;
    pkt.data_size = 0;
    return pkt;
}
}
