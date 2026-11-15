#pragma once
#include <optional>
#include <string>
#include <string_view>

// Small text helpers the core needs (the proxy's own features are modules).
// Kept free of network code so they can be unit tested.
namespace core::features {
// Growtopia world names: 1-23 letters or digits. Returns the upper-case name.
[[nodiscard]] std::optional<std::string> normalize_world_name(std::string_view name);

// Removes Growtopia colour codes (`w, ``, `4 ...) from a name.
[[nodiscard]] std::string strip_colors(std::string_view text);

// Text that can be put inside dialog markup or a "key|value" line: drops the
// characters the game uses as separators ('|', newlines).
[[nodiscard]] std::string sanitize(std::string_view text);
}
