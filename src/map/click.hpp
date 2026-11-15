#pragma once
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include <glm/glm.hpp>

#include "coords.hpp"

namespace map {
enum class MouseButton : std::uint8_t {
    Left,
    Middle,
    Right,
};

enum class ClickAction : std::uint8_t {
    Walk,       // left button: go to the tile
    Inspect,    // right button: can you get there from where you stand
};

// A click in the map window: where the cursor was and what the canvas showed
// at that moment.
struct Click {
    glm::vec2 screen{ 0.0f };
    MouseButton button{ MouseButton::Left };
    Viewport view;
};

struct ClickTarget {
    TilePos tile;
    ClickAction action{ ClickAction::Walk };
};

enum class ClickError {
    NoWorld,        // not in a world (nothing loaded yet)
    BadView,        // the window sent an impossible view (zoom 0, size 0 ...)
    OutsideWorld,   // the cursor was not over the world
    NoAction,       // a button that does nothing
};

[[nodiscard]] std::string_view describe(ClickError error);

// Reads a click from the window's request parameters:
//   x, y      cursor position in the canvas (CSS pixels)
//   button    0 = left, 1 = middle, 2 = right (MouseEvent.button)
//   ox, oy    world point at the canvas top-left corner
//   zoom      canvas pixels per world pixel
//   w, h      canvas size
// Nothing when a value is missing or not a number.
[[nodiscard]] std::optional<Click> parse_click(const std::multimap<std::string, std::string>& params);

[[nodiscard]] std::optional<ClickAction> action_for(MouseButton button);

// Tile typed by hand ("/goto 12 30", "reach 12 30" in the proxy console).
[[nodiscard]] std::optional<TilePos> parse_tile(std::string_view x, std::string_view y);

// The click handler's first step: cursor -> tile, button -> action.
[[nodiscard]] std::expected<ClickTarget, ClickError> resolve_click(const Click& click, glm::ivec2 world_size);
}
