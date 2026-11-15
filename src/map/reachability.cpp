#include "reachability.hpp"

#include <array>
#include <deque>
#include <vector>

namespace map {
namespace {
constexpr std::array<glm::ivec2, 4> NEIGHBOURS{ {
    { 1, 0 }, { -1, 0 }, { 0, -1 }, { 0, 1 },
} };

// Can the body stand in this tile: every tile it covers is free (a door only
// as the target itself).
bool fits(const Grid& grid, const TilePos tile, const Body& body, const TilePos target)
{
    const auto [first, last]{ covered_tiles(tile, body) };
    for (int y{ first.y }; y <= last.y; ++y) {
        for (int x{ first.x }; x <= last.x; ++x) {
            const TilePos covered{ x, y };
            if (!grid.contains(covered)) {
                return false;
            }

            const Cell cell{ grid.at(covered) };
            if (is_passable(cell) || (cell == Cell::Door && covered == target && tile == target)) {
                continue;
            }
            return false;
        }
    }
    return true;
}
}

Body Body::from_colrect(const glm::ivec4& colrect)
{
    if (colrect.z <= 0 || colrect.w <= 0) {
        return Body{};
    }
    return Body{ { colrect.x, colrect.y }, { colrect.z, colrect.w } };
}

TileSpan covered_tiles(const TilePos tile, const Body& body)
{
    const glm::vec2 origin{ tile_to_world(tile) + glm::vec2{ body.offset } };
    // Last pixel inside the box (a box is at least one pixel big).
    const glm::vec2 end{ origin + glm::vec2{ glm::max(body.size, glm::ivec2{ 1 }) - glm::ivec2{ 1 } } };
    return { world_to_tile(origin), world_to_tile(end) };
}

TilePos body_tile(const glm::vec2 position, const Body& body)
{
    return world_to_tile(position + glm::vec2{ body.offset } + glm::vec2{ body.size } * 0.5f);
}

std::string_view headline(const Reach reach)
{
    switch (reach) {
    case Reach::Reachable: return "Цель доступна";
    case Reach::Unreachable:
    case Reach::TargetBlocked:
    case Reach::OutOfBounds: return "Цель недоступна";
    case Reach::NoData: return "Нет данных";
    }
    return "Нет данных";
}

std::string_view reason(const Reach reach)
{
    switch (reach) {
    case Reach::TargetBlocked: return "в клетке блок";
    case Reach::OutOfBounds: return "за пределами карты";
    default: return {};
    }
}

std::string_view steps_word(const std::size_t steps)
{
    const auto last_two{ steps % 100 };
    const auto last{ steps % 10 };
    if (last_two >= 11 && last_two <= 14) {
        return "шагов";
    }
    if (last == 1) {
        return "шаг";
    }
    if (last >= 2 && last <= 4) {
        return "шага";
    }
    return "шагов";
}

std::string_view code(const Reach reach)
{
    switch (reach) {
    case Reach::Reachable: return "reachable";
    case Reach::Unreachable: return "unreachable";
    case Reach::TargetBlocked: return "blocked";
    case Reach::OutOfBounds: return "out_of_bounds";
    case Reach::NoData: return "no_data";
    }
    return "no_data";
}

ReachResult check_reach(
    const Grid& grid,
    const std::optional<glm::vec2> position,
    const Body& body,
    const TilePos target
)
{
    ReachResult result{};
    if (grid.empty() || !position) {
        return result; // NoData
    }

    const TilePos start{ body_tile(*position, body) };
    if (!grid.contains(start)) {
        // A position outside the world means it is stale or wrong.
        return result;
    }
    result.start = start;

    if (!grid.contains(target)) {
        result.status = Reach::OutOfBounds;
        return result;
    }

    if (!fits(grid, target, body, target)) {
        result.status = Reach::TargetBlocked;
        return result;
    }

    if (start == target) {
        result.status = Reach::Reachable;
        return result;
    }

    // You are standing where you are, whatever the map says about that tile
    // (a door you spawned on, a platform): the search starts there.
    const int width{ grid.size().x };
    std::vector<int> distance(grid.cells().size(), -1);
    std::deque<TilePos> queue{ start };
    distance[tile_index(start, width)] = 0;

    while (!queue.empty()) {
        const TilePos tile{ queue.front() };
        queue.pop_front();
        const int here{ distance[tile_index(tile, width)] };

        for (const auto step : NEIGHBOURS) {
            const TilePos next{ tile.x + step.x, tile.y + step.y };
            if (!grid.contains(next) || distance[tile_index(next, width)] >= 0) {
                continue;
            }
            if (!fits(grid, next, body, target)) {
                continue;
            }

            distance[tile_index(next, width)] = here + 1;
            if (next == target) {
                result.status = Reach::Reachable;
                result.steps = static_cast<std::size_t>(here + 1);
                return result;
            }
            queue.push_back(next);
        }
    }

    result.status = Reach::Unreachable;
    return result;
}

std::vector<int> reach_distances(const Grid& grid, const glm::vec2 position, const Body& body)
{
    std::vector<int> distance(grid.cells().size(), -1);
    const TilePos start{ body_tile(position, body) };
    if (grid.empty() || !grid.contains(start)) {
        return distance;
    }

    const int width{ grid.size().x };
    const TilePos nowhere{ -1, -1 };
    std::deque<TilePos> queue{ start };
    distance[tile_index(start, width)] = 0;
    while (!queue.empty()) {
        const TilePos tile{ queue.front() };
        queue.pop_front();
        const int here{ distance[tile_index(tile, width)] };
        for (const auto step : NEIGHBOURS) {
            const TilePos next{ tile.x + step.x, tile.y + step.y };
            if (!grid.contains(next) || distance[tile_index(next, width)] >= 0 || !fits(grid, next, body, nowhere)) {
                continue;
            }
            distance[tile_index(next, width)] = here + 1;
            queue.push_back(next);
        }
    }
    return distance;
}

std::vector<TilePos> reach_path(const Grid& grid, const glm::vec2 position, const Body& body, const TilePos target)
{
    const TilePos start{ body_tile(position, body) };
    if (grid.empty() || !grid.contains(start) || !grid.contains(target) || !fits(grid, target, body, target)) {
        return {};
    }
    if (start == target) {
        return { start };
    }

    const int width{ grid.size().x };
    std::vector<int> from(grid.cells().size(), -2);     // -2 unseen, -1 start
    std::deque<TilePos> queue{ start };
    from[tile_index(start, width)] = -1;
    while (!queue.empty()) {
        const TilePos tile{ queue.front() };
        queue.pop_front();
        for (const auto step : NEIGHBOURS) {
            const TilePos next{ tile.x + step.x, tile.y + step.y };
            if (!grid.contains(next) || from[tile_index(next, width)] != -2 || !fits(grid, next, body, target)) {
                continue;
            }
            from[tile_index(next, width)] = static_cast<int>(tile_index(tile, width));
            if (next == target) {
                std::vector<TilePos> path{ next };
                for (int at{ from[tile_index(next, width)] }; at >= 0; at = from[static_cast<std::size_t>(at)]) {
                    path.push_back(TilePos{ at % width, at / width });
                }
                return { path.rbegin(), path.rend() };
            }
            queue.push_back(next);
        }
    }
    return {};
}
}
