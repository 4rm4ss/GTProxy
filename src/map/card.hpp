#pragma once
#include <optional>
#include <string>

#include "coords.hpp"

namespace map {
// What the map window shows as the answer to a click: a card (title, text)
// and optionally a marker on the tile.
struct Card {
    std::string title;
    std::string text;
    std::string kind{ "none" };     // "ok" (green), "bad" (red), "none" (grey)
    std::string status;             // free tag for tools, e.g. "reachable"
    bool marker{ true };
    bool toast{ false };            // a short note at the bottom instead of a card
    std::optional<TilePos> tile;    // the clicked tile if not given
};
}
