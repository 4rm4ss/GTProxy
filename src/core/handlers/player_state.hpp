#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "../../packet/packet_types.hpp"

// The packets behind your character's abilities, as bytes: the server's
// character state (PACKET_SET_CHARACTER_STATE) and the game's state reports
// (PACKET_STATE). Free of the network so they can be tested.
namespace core::player_state {
struct Abilities {
    float speed{ 1.0f };            // x the server's walking speed
    float gravity{ 1.0f };          // x the server's gravity (lower: higher, longer jumps)
    float acceleration{ 1.0f };     // x the server's acceleration
    float water_speed{ 1.0f };      // x the server's speed in water
    bool double_jump{ false };
    bool noclip{ false };           // walk through blocks
    int range{ 0 };                 // added to the punch and build range
    bool god{ false };

    bool operator==(const Abilities&) const = default;
    [[nodiscard]] bool changes_state() const;
};

inline constexpr float MIN_FACTOR{ 0.05f };
inline constexpr float MAX_FACTOR{ 20.0f };
inline constexpr int MAX_RANGE{ 100 };

// Bits of a state report's flags the game sets when it is hurt.
inline constexpr std::uint32_t FIRE_DAMAGE{ 1u << 6 };
inline constexpr std::uint32_t KILLED{ 1u << 8 };
inline constexpr std::uint32_t DAMAGE{ 1u << 16 };
inline constexpr std::uint32_t ACID_DAMAGE{ 1u << 25 };
inline constexpr std::uint32_t HURT{ FIRE_DAMAGE | KILLED | DAMAGE | ACID_DAMAGE };

// Bits of the character state (int_data).
inline constexpr std::int32_t WALK_THROUGH_BLOCKS{ 1 << 0 };
inline constexpr std::int32_t DOUBLE_JUMP{ 1 << 1 };

[[nodiscard]] std::optional<std::uint32_t> message_type(std::span<const std::byte> data);
[[nodiscard]] std::optional<packet::GameUpdatePacket> game_header(std::span<const std::byte> data);

// Values within their limits.
[[nodiscard]] Abilities clamped(Abilities abilities);
// A character state packet with the abilities applied (anything else unchanged).
[[nodiscard]] std::vector<std::byte> apply(std::vector<std::byte> packet, const Abilities& abilities);
// The character state a server usually sends a player without items.
[[nodiscard]] std::vector<std::byte> usual_state(std::int32_t net_id);
// A state report with the damage taken out; nullopt if it reports none.
[[nodiscard]] std::optional<std::vector<std::byte>> without_damage(std::span<const std::byte> packet);
// Growtopia's own servers (by name or address).
[[nodiscard]] bool official_address(std::string_view address);
}
