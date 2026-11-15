#pragma once
#include <optional>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include "feature_commands.hpp"
#include "../../core/browser.hpp"
#include "../../core/handlers/map_handler.hpp"
#include "../../map/click.hpp"

// Commands of the map window and walking (core::handlers::MapHandler).
namespace command {
class MapFeatureCommand : public FeatureCommand {
public:
    MapFeatureCommand(core::handlers::FeatureHandler& features, core::handlers::MapHandler& map)
        : FeatureCommand{ features }
        , map_{ map }
    { }

protected:
    core::handlers::MapHandler& map_;
};

class MapCommand final : public MapFeatureCommand {
public:
    using MapFeatureCommand::MapFeatureCommand;

    [[nodiscard]] std::string_view name() const override { return "map"; }
    [[nodiscard]] std::string description() const override { return "Open the map window: click a tile to walk there"; }

    Result execute(const Context&) override
    {
        const auto& url{ map_.window_url() };
        if (!url) {
            reply("`4Oops: ``The map window is off or its port is busy (see the proxy log, config.json \"map\").");
            return Result::Failed;
        }

        const bool opened{ core::browser::open_window(*url) };
        reply(fmt::format(
            "`oMap window: `w{}``{} Left click walks, right click shows a tile.",
            *url,
            opened ? "" : " (open it in a browser)"
        ));
        return Result::Success;
    }
};

class GotoCommand final : public MapFeatureCommand {
public:
    using MapFeatureCommand::MapFeatureCommand;

    [[nodiscard]] std::string_view name() const override { return "goto"; }
    [[nodiscard]] std::string description() const override { return "Walk to a tile: goto <x> <y>; goto stop"; }

    Result execute(const Context& ctx) override
    {
        if (ctx.args.size() == 1 && ctx.args[0] == "stop") {
            reply(fmt::format("`o[map] {}", map_.stop().message));
            return Result::Success;
        }

        const auto tile{ ctx.args.size() == 2 ? map::parse_tile(ctx.args[0], ctx.args[1]) : std::nullopt };
        if (!tile) {
            reply(fmt::format("`4Usage: ``{0}goto <x> <y> (tile, as {0}pos shows it); {0}goto stop", ctx.registry.prefix()));
            return Result::InvalidArguments;
        }

        const auto result{ map_.walk_to(*tile) };
        reply(fmt::format("{}[map] {}", result.ok ? "`o" : "`4", result.message));
        return result.ok ? Result::Success : Result::Failed;
    }
};

// /reach <x> <y>: the right click check typed by hand. The game's font has
// no Cyrillic, so the game gets the result in English; the proxy log has
// it as in the map window ("Цель доступна" ...).
class ReachCommand final : public MapFeatureCommand {
public:
    using MapFeatureCommand::MapFeatureCommand;

    [[nodiscard]] std::string_view name() const override { return "reach"; }
    [[nodiscard]] std::string description() const override { return "Can you get to a tile from where you stand: reach <x> <y>"; }

    Result execute(const Context& ctx) override
    {
        const auto tile{ ctx.args.size() == 2 ? map::parse_tile(ctx.args[0], ctx.args[1]) : std::nullopt };
        if (!tile) {
            reply(fmt::format("`4Usage: ``{0}reach <x> <y> (tile, as {0}pos shows it)", ctx.registry.prefix()));
            return Result::InvalidArguments;
        }

        const auto result{ map_.check_reach(*tile, "/reach in the game") };
        reply(fmt::format("[map] {}, {}: {}", tile->x, tile->y, in_english(result)));
        return Result::Success;
    }

private:
    static std::string in_english(const core::handlers::MapHandler::Result& result)
    {
        if (result.reach == "reachable") {
            return "`2target reachable``";
        }
        if (result.reach == "unreachable") {
            return "`4target unreachable``";
        }
        if (result.reach == "blocked") {
            return "`4target unreachable: a block is there``";
        }
        if (result.reach == "out_of_bounds") {
            return "`4target unreachable: outside the map``";
        }
        return "`5no data`` (no world, items.dat or position yet - see the proxy log)";
    }
};

class ClickWalkCommand final : public MapFeatureCommand {
public:
    using MapFeatureCommand::MapFeatureCommand;

    [[nodiscard]] std::string_view name() const override { return "clickwalk"; }
    [[nodiscard]] std::string description() const override { return "Toggle: clicking a tile in the game walks there instead of punching"; }

    Result execute(const Context& ctx) override
    {
        map_.set_click_walk(!map_.click_walk());
        reply(map_.click_walk()
            ? fmt::format("`oClick-walk `2on``: clicking a tile walks there (no punching or building). {}clickwalk again to turn it off.", ctx.registry.prefix())
            : std::string{ "`oClick-walk `4off``: clicks punch and build again." });
        return Result::Success;
    }
};
}
