#pragma once
#include <optional>
#include <string>
#include <string_view>

// Small text helpers behind the proxy's own features (/warp, /nick, the
// /proxy menu). Kept free of network code so they can be unit tested.
namespace core::features {
// Growtopia world names: 1-23 letters or digits. Returns the upper-case name.
[[nodiscard]] std::optional<std::string> normalize_world_name(std::string_view name);

// Door ids inside a world (the part after '|' in "WORLD|DOOR"): 1-24 letters,
// digits or '_'. Returns the upper-case id.
[[nodiscard]] std::optional<std::string> normalize_door_id(std::string_view door);

// Replaces the value of one "key|value" line of OnSpawn-style text and keeps
// every other line (including fields this proxy does not know) unchanged.
// Returns nullopt when the key is not present.
[[nodiscard]] std::optional<std::string> replace_field(std::string_view text, std::string_view key, std::string_view value);

// Value of one "key|value" line (everything after the first '|').
[[nodiscard]] std::optional<std::string> field_value(std::string_view text, std::string_view key);

// Removes Growtopia colour codes (`w, ``, `4 ...) from a name.
[[nodiscard]] std::string strip_colors(std::string_view text);

// Text that can be put inside dialog markup or a "key|value" line: drops the
// characters the game uses as separators ('|', newlines).
[[nodiscard]] std::string sanitize(std::string_view text);

// Name as the game shows it: plain text becomes white ("`wName``"); text that
// already has colour codes is kept as typed.
[[nodiscard]] std::string colored_name(std::string_view name);
}
