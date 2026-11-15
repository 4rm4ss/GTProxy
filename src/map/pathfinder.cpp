#include "pathfinder.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <queue>

namespace map {
namespace {
constexpr int STRAIGHT_COST{ 10 };
constexpr int DIAGONAL_COST{ 14 };
constexpr std::uint32_t NO_PARENT{ std::numeric_limits<std::uint32_t>::max() };
constexpr int UNREACHED{ std::numeric_limits<int>::max() };

struct Move {
    int dx;
    int dy;
};

constexpr std::array<Move, 8> MOVES{ {
    { 1, 0 }, { -1, 0 }, { 0, -1 }, { 0, 1 },
    { 1, -1 }, { -1, -1 }, { 1, 1 }, { -1, 1 },
} };

// Admissible estimate for 8-directional moves.
int octile(const TilePos a, const TilePos b)
{
    const int dx{ std::abs(a.x - b.x) };
    const int dy{ std::abs(a.y - b.y) };
    return STRAIGHT_COST * std::max(dx, dy) + (DIAGONAL_COST - STRAIGHT_COST) * std::min(dx, dy);
}

// Air state in gravity mode: 0 = standing on something, 1..jump = moves
// made since leaving the ground, fall = only falling until you land.
class AirModel {
public:
    // 'ceiling': how high a rise can matter at all (the world's height), so
    // 1000 jumps do not make the search a thousand times bigger.
    AirModel(const PathOptions& options, const int ceiling)
        : jump_{ options.gravity ? std::clamp(std::max(options.jump_height, 0) * std::max(options.air_jumps, 1), 0, std::max(ceiling, 1)) : 0 }
        , gravity_{ options.gravity }
    { }

    [[nodiscard]] int states() const { return gravity_ ? jump_ + 2 : 1; }
    [[nodiscard]] int fall() const { return jump_ + 1; }

    // Air state after the move, or -1 if the move is impossible from 'air'.
    // 'supported' tells whether there is ground under the tile moved to.
    [[nodiscard]] int next(const int air, const Move move, const bool supported) const
    {
        if (!gravity_) {
            return 0;
        }

        if (move.dy > 0) {
            return supported ? 0 : fall();
        }

        if (air == fall()) {
            return -1;
        }

        if (move.dy < 0) {
            if (air >= jump_) {
                return -1;
            }
            return supported ? 0 : air + 1;
        }

        // Sideways: walking along the ground, off a ledge, or steering in
        // the air (which uses up the jump like rising does).
        if (supported) {
            return 0;
        }
        return air + 1 > jump_ ? fall() : air + 1;
    }

private:
    int jump_;
    bool gravity_;
};
}

std::string_view describe(const PathError error)
{
    switch (error) {
    case PathError::EmptyWorld: return "no world loaded";
    case PathError::StartOutside: return "your position is outside the world";
    case PathError::GoalOutside: return "that tile is outside the world";
    case PathError::GoalBlocked: return "that tile is blocked and there is no free tile next to it";
    case PathError::NoPath: return "there is no way to get there";
    }
    return "unknown error";
}

std::expected<Path, PathError> find_path(
    const Grid& grid,
    const TilePos start,
    const TilePos goal,
    const PathOptions& options
)
{
    if (grid.empty()) {
        return std::unexpected{ PathError::EmptyWorld };
    }
    if (!grid.contains(start)) {
        return std::unexpected{ PathError::StartOutside };
    }
    if (!grid.contains(goal)) {
        return std::unexpected{ PathError::GoalOutside };
    }
    if (start == goal) {
        return Path{ start };
    }
    if (!can_end_in(grid.at(goal))) {
        return std::unexpected{ PathError::GoalBlocked };
    }

    const AirModel air{ options, grid.size().y + 1 };
    const int width{ grid.size().x };
    const auto states{ static_cast<std::uint32_t>(air.states()) };
    const std::size_t node_count{ grid.cells().size() * states };

    const auto node_of = [&](const TilePos tile, const int state) {
        return static_cast<std::uint32_t>(tile_index(tile, width) * states + static_cast<std::uint32_t>(state));
    };
    const auto tile_of = [&](const std::uint32_t node) {
        const auto index{ node / states };
        return TilePos{ static_cast<int>(index % static_cast<std::uint32_t>(width)), static_cast<int>(index / static_cast<std::uint32_t>(width)) };
    };
    const auto enterable = [&](const TilePos tile) {
        const Cell cell{ grid.at(tile) };
        return is_passable(cell) || (tile == goal && cell == Cell::Door);
    };
    const auto supported = [&](const TilePos tile) {
        return can_stand_on(grid.at(TilePos{ tile.x, tile.y + 1 }));
    };

    std::vector<int> cost(node_count, UNREACHED);
    std::vector<std::uint32_t> parent(node_count, NO_PARENT);
    std::vector<bool> closed(node_count, false);

    struct Open {
        int estimate;
        int cost;
        std::uint32_t node;
        bool operator>(const Open& other) const
        {
            // Among equal estimates, prefer the node further along.
            return estimate != other.estimate ? estimate > other.estimate : cost < other.cost;
        }
    };
    std::priority_queue<Open, std::vector<Open>, std::greater<>> open;

    const int start_air{ options.gravity && !supported(start) ? air.fall() : 0 };
    const auto start_node{ node_of(start, start_air) };
    cost[start_node] = 0;
    open.push({ octile(start, goal), 0, start_node });

    std::size_t expanded{ 0 };
    while (!open.empty()) {
        const Open current{ open.top() };
        open.pop();

        if (closed[current.node] || current.cost != cost[current.node]) {
            continue;
        }
        closed[current.node] = true;

        const TilePos tile{ tile_of(current.node) };
        if (tile == goal) {
            Path path;
            for (auto node{ current.node }; node != NO_PARENT; node = parent[node]) {
                path.push_back(tile_of(node));
            }
            std::ranges::reverse(path);
            return path;
        }

        if (++expanded > options.max_expanded) {
            break;
        }

        const int state{ static_cast<int>(current.node % states) };
        for (const Move move : MOVES) {
            const TilePos next{ tile.x + move.dx, tile.y + move.dy };
            if (!grid.contains(next) || !enterable(next)) {
                continue;
            }

            const bool diagonal{ move.dx != 0 && move.dy != 0 };
            // No cutting corners: both tiles beside a diagonal step must be free.
            if (diagonal && (
                !is_passable(grid.at(TilePos{ tile.x + move.dx, tile.y }))
                || !is_passable(grid.at(TilePos{ tile.x, tile.y + move.dy }))
            )) {
                continue;
            }

            const int next_state{ air.next(state, move, supported(next)) };
            if (next_state < 0) {
                continue;
            }

            const auto next_node{ node_of(next, next_state) };
            if (closed[next_node]) {
                continue;
            }

            const int next_cost{ current.cost + (diagonal ? DIAGONAL_COST : STRAIGHT_COST) };
            if (next_cost >= cost[next_node]) {
                continue;
            }

            cost[next_node] = next_cost;
            parent[next_node] = current.node;
            open.push({ next_cost + octile(next, goal), next_cost, next_node });
        }
    }

    return std::unexpected{ PathError::NoPath };
}

std::expected<Destination, PathError> find_destination(
    const Grid& grid,
    const TilePos start,
    const TilePos goal,
    const PathOptions& options
)
{
    if (!grid.empty() && grid.contains(goal) && !can_end_in(grid.at(goal))) {
        // Tiles around the block, nearest to you first.
        std::vector<TilePos> around;
        for (const Move move : MOVES) {
            const TilePos tile{ goal.x + move.dx, goal.y + move.dy };
            if (grid.contains(tile) && is_passable(grid.at(tile))) {
                around.push_back(tile);
            }
        }

        if (around.empty()) {
            return std::unexpected{ PathError::GoalBlocked };
        }

        std::ranges::stable_sort(around, {}, [&](const TilePos tile) { return octile(tile, start); });
        for (const TilePos tile : around) {
            if (auto path{ find_path(grid, start, tile, options) }) {
                return Destination{ tile, std::move(*path), true };
            }
        }

        return std::unexpected{ PathError::NoPath };
    }

    auto path{ find_path(grid, start, goal, options) };
    if (!path) {
        return std::unexpected{ path.error() };
    }

    return Destination{ goal, std::move(*path), false };
}
}
