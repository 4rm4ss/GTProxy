#pragma once
#include <charconv>
#include <cmath>
#include <string>
#include <fmt/format.h>
#include <magic_enum/magic_enum.hpp>

#include "../command.hpp"
#include "../command_registry.hpp"
#include "../../core/features.hpp"
#include "../../core/handlers/feature_handler.hpp"
#include "../../item/item_database.hpp"
#include "../../world/world.hpp"

// Commands built on core::handlers::FeatureHandler. They only change what
// your own game shows, or send what the game itself sends when you exit a
// world and type a world name.
namespace command {
class FeatureCommand : public ICommand {
public:
    explicit FeatureCommand(core::handlers::FeatureHandler& features)
        : features_{ features }
    { }

protected:
    void reply(const std::string& text) const { features_.message(text); }

    core::handlers::FeatureHandler& features_;
};

class WarpCommand final : public FeatureCommand {
public:
    using FeatureCommand::FeatureCommand;

    [[nodiscard]] std::string_view name() const override { return "warp"; }
    [[nodiscard]] std::string description() const override { return "Go to a world: warp <world> [door id]"; }

    Result execute(const Context& ctx) override
    {
        if (ctx.args.empty() || ctx.args.size() > 2) {
            reply(fmt::format("`4Usage: ``{}warp <world> [door id]", ctx.registry.prefix()));
            return Result::InvalidArguments;
        }

        // Also accept the game's own "WORLD|DOOR" spelling.
        std::string world{ ctx.args[0] };
        std::string door{ ctx.args.size() > 1 ? ctx.args[1] : std::string{} };
        if (const auto bar{ world.find('|') }; bar != std::string::npos && door.empty()) {
            door = world.substr(bar + 1);
            world.resize(bar);
        }

        const auto world_name{ core::features::normalize_world_name(world) };
        if (!world_name || *world_name == "EXIT") {
            reply("`4Oops: ``World names are 1-23 letters or digits.");
            return Result::InvalidArguments;
        }

        std::string target{ *world_name };
        if (!door.empty()) {
            const auto door_id{ core::features::normalize_door_id(door) };
            if (!door_id) {
                reply("`4Oops: ``Door ids are letters, digits or _ (up to 24).");
                return Result::InvalidArguments;
            }
            target += '|' + *door_id;
        }

        return go(features_, target) ? Result::Success : Result::Failed;
    }

    static bool go(core::handlers::FeatureHandler& features, const std::string& target)
    {
        if (!features.warp(target)) {
            features.message("`4Oops: ``Not connected to the game server.");
            return false;
        }
        features.message(fmt::format("`oWarping to `w{}``...", target));
        return true;
    }
};

class BackCommand final : public FeatureCommand {
public:
    using FeatureCommand::FeatureCommand;

    [[nodiscard]] std::string_view name() const override { return "back"; }
    [[nodiscard]] std::string description() const override { return "Go back to the previous world"; }

    Result execute(const Context&) override
    {
        if (features_.previous_world().empty()) {
            reply("`4Oops: ``No previous world yet.");
            return Result::Failed;
        }
        return WarpCommand::go(features_, features_.previous_world()) ? Result::Success : Result::Failed;
    }
};

class RejoinCommand final : public FeatureCommand {
public:
    using FeatureCommand::FeatureCommand;

    [[nodiscard]] std::string_view name() const override { return "rejoin"; }
    [[nodiscard]] std::string description() const override { return "Exit and enter the current world again"; }

    Result execute(const Context&) override
    {
        if (!features_.in_world() || features_.current_world().empty()) {
            reply("`4Oops: ``You are not in a world.");
            return Result::Failed;
        }
        return WarpCommand::go(features_, features_.current_world()) ? Result::Success : Result::Failed;
    }
};

class NickCommand final : public FeatureCommand {
public:
    using FeatureCommand::FeatureCommand;

    [[nodiscard]] std::string_view name() const override { return "nick"; }
    [[nodiscard]] std::string description() const override { return "Show another name above you (only you see it); no name resets"; }

    Result execute(const Context& ctx) override
    {
        if (ctx.args.empty()) {
            features_.reset_nick();
            reply("`oNick reset to your real name.");
            return Result::Success;
        }

        std::string nick{ ctx.args.front() };
        for (std::size_t i{ 1 }; i < ctx.args.size(); ++i) {
            nick += ' ' + ctx.args[i];
        }

        nick = core::features::sanitize(nick);
        if (core::features::strip_colors(nick).empty() || nick.size() > 64) {
            reply("`4Oops: ``Nick must have 1-64 characters.");
            return Result::InvalidArguments;
        }

        features_.set_nick(nick);
        reply(fmt::format("`oNick set to {}`` (only you see it; {}nick resets).", core::features::colored_name(nick), ctx.registry.prefix()));
        return Result::Success;
    }
};

class NetIdCommand final : public FeatureCommand {
public:
    using FeatureCommand::FeatureCommand;

    [[nodiscard]] std::string_view name() const override { return "netid"; }
    [[nodiscard]] std::string description() const override { return "Show/hide netIDs next to player names"; }

    Result execute(const Context&) override
    {
        features_.set_show_net_ids(!features_.show_net_ids());
        reply(features_.show_net_ids() ? "`oNetIDs shown next to names." : "`oNetIDs hidden.");
        return Result::Success;
    }
};

class PosCommand final : public FeatureCommand {
public:
    using FeatureCommand::FeatureCommand;

    [[nodiscard]] std::string_view name() const override { return "pos"; }
    [[nodiscard]] std::string description() const override { return "Show your position (tile x, y)"; }

    Result execute(const Context&) override
    {
        const auto pos{ features_.local_position() };
        if (!features_.in_world() || !pos) {
            reply("`4Oops: ``Position unknown - enter a world and move a bit.");
            return Result::Failed;
        }

        reply(fmt::format(
            "`oYou are at tile `w{}, {}`` in `w{}``.",
            static_cast<int>(std::floor(pos->x / 32.0f)),
            static_cast<int>(std::floor(pos->y / 32.0f)),
            features_.current_world()
        ));
        return Result::Success;
    }
};

class PlayersCommand final : public FeatureCommand {
public:
    using FeatureCommand::FeatureCommand;

    [[nodiscard]] std::string_view name() const override { return "players"; }
    [[nodiscard]] std::string description() const override { return "List the players you can see in this world"; }

    Result execute(const Context&) override
    {
        if (!features_.in_world()) {
            reply("`4Oops: ``You are not in a world.");
            return Result::Failed;
        }

        // Only players the game shows: invisible ones stay hidden.
        int shown{ 0 };
        std::string lines;
        for (const auto& [net_id, player] : world::World::instance().get_players()) {
            if (player->invisible() != 0) {
                continue;
            }
            ++shown;
            lines += fmt::format(
                "\n`o[{}] {}`` `o{}{}",
                net_id,
                player->name(),
                player->country_code().empty() ? "" : "(" + player->country_code() + ")",
                player->is_local() ? " `2(you)``" : ""
            );
        }

        reply(fmt::format("`o{} player(s) in `w{}``:{}", shown, features_.current_world(), lines));
        return Result::Success;
    }
};

class WorldInfoCommand final : public FeatureCommand {
public:
    using FeatureCommand::FeatureCommand;

    [[nodiscard]] std::string_view name() const override { return "world"; }
    [[nodiscard]] std::string description() const override { return "Show information about this world"; }

    Result execute(const Context&) override
    {
        if (!features_.in_world()) {
            reply("`4Oops: ``You are not in a world.");
            return Result::Failed;
        }

        auto& world{ world::World::instance() };
        const auto size{ world.get_tile_map().get_size() };
        int visible{ 0 };
        for (const auto& [net_id, player] : world.get_players()) {
            visible += player->invisible() == 0 ? 1 : 0;
        }

        reply(fmt::format(
            "`oWorld `w{}``: {}x{} tiles, {} player(s), {} dropped item stack(s). Previous world: `w{}``.",
            features_.current_world(),
            size.x,
            size.y,
            visible,
            world.get_object_map().get_objects().size(),
            features_.previous_world().empty() ? "-" : features_.previous_world()
        ));
        return Result::Success;
    }
};

class FindItemCommand final : public FeatureCommand {
public:
    using FeatureCommand::FeatureCommand;

    [[nodiscard]] std::string_view name() const override { return "find"; }
    [[nodiscard]] std::string description() const override { return "Find items by name: find <text>"; }

    Result execute(const Context& ctx) override
    {
        if (ctx.args.empty()) {
            reply(fmt::format("`4Usage: ``{}find <item name>", ctx.registry.prefix()));
            return Result::InvalidArguments;
        }

        const auto& items{ item::ItemDatabase::instance() };
        if (items.empty()) {
            reply("`4Oops: ``items.dat is not loaded yet - log in first.");
            return Result::Failed;
        }

        std::string query{ ctx.args.front() };
        for (std::size_t i{ 1 }; i < ctx.args.size(); ++i) {
            query += ' ' + ctx.args[i];
        }

        constexpr std::size_t LIMIT{ 12 };
        const auto found{ items.find(query, LIMIT) };
        if (found.empty()) {
            reply(fmt::format("`oNo items match '{}'.", core::features::sanitize(query)));
            return Result::Success;
        }

        std::string lines;
        for (const auto* info : found) {
            lines += fmt::format("\n`o{}: `w{}``", info->item_id, info->item_name);
        }
        reply(fmt::format("`oItems matching '{}'{}:{}", core::features::sanitize(query), found.size() == LIMIT ? " (first 12)" : "", lines));
        return Result::Success;
    }
};

class ItemInfoCommand final : public FeatureCommand {
public:
    using FeatureCommand::FeatureCommand;

    [[nodiscard]] std::string_view name() const override { return "item"; }
    [[nodiscard]] std::string description() const override { return "Show an item: item <id>"; }

    Result execute(const Context& ctx) override
    {
        std::uint32_t id{};
        if (
            ctx.args.size() != 1
            || std::from_chars(ctx.args[0].data(), ctx.args[0].data() + ctx.args[0].size(), id).ec != std::errc{}
        ) {
            reply(fmt::format("`4Usage: ``{}item <id>", ctx.registry.prefix()));
            return Result::InvalidArguments;
        }

        const auto* info{ item::ItemDatabase::instance().get_item(id) };
        if (!info) {
            reply(fmt::format("`4Oops: ``No item {} (is items.dat loaded?).", id));
            return Result::Failed;
        }

        reply(fmt::format(
            "`o{}: `w{}`` - type {}, rarity {}, stacks up to {}",
            info->item_id,
            info->item_name,
            magic_enum::enum_name(info->item_type),
            info->rarity,
            info->max_amount
        ));
        return Result::Success;
    }
};

// /proxy: menu with the proxy's features (a game dialog only you see).
class ProxyMenuCommand final : public FeatureCommand {
public:
    static constexpr std::string_view DIALOG_NAME{ "gtproxy_menu" };

    using FeatureCommand::FeatureCommand;

    [[nodiscard]] std::string_view name() const override { return "proxy"; }
    [[nodiscard]] std::string description() const override { return "Open the proxy menu"; }

    Result execute(const Context& ctx) override
    {
        features_.show_dialog(build(features_, ctx.registry));
        return Result::Success;
    }

    [[nodiscard]] static std::string build(const core::handlers::FeatureHandler& features, const CommandRegistry& registry)
    {
        using core::features::sanitize;

        std::string status;
        if (features.in_world()) {
            status = fmt::format("World: `w{}``", sanitize(features.current_world()));
            if (const auto pos{ features.local_position() }) {
                status += fmt::format("  Position: `w{}, {}``", static_cast<int>(pos->x / 32.0f), static_cast<int>(pos->y / 32.0f));
            }
        } else {
            status = "Not in a world";
        }

        std::string d;
        d += "set_default_color|`o\n";
        d += "add_label_with_icon|big|`wGTProxy``|left|32|\n";
        d += fmt::format("add_smalltext|{}|\n", status);
        d += "add_spacer|small|\n";

        d += "add_textbox|`wWorlds``|left|\n";
        d += "add_text_input|world|World (WORLD or WORLD DOOR):||30|\n";
        d += "add_button|warp|Warp|noflags|0|0|\n";
        if (!features.previous_world().empty()) {
            d += fmt::format("add_button|back|Back to {}|noflags|0|0|\n", sanitize(features.previous_world()));
        }
        if (features.in_world()) {
            d += "add_button|rejoin|Rejoin this world|noflags|0|0|\n";
        }
        d += "add_spacer|small|\n";

        d += "add_textbox|`wLooks (only you see these)``|left|\n";
        d += fmt::format("add_text_input|nick|Nick:|{}|64|\n", sanitize(features.nick()));
        d += "add_button|set_nick|Set nick|noflags|0|0|\n";
        if (!features.nick().empty()) {
            d += "add_button|reset_nick|Reset nick|noflags|0|0|\n";
        }
        d += fmt::format("add_checkbox|show_netid|Show netIDs next to names|{}|\n", features.show_net_ids() ? 1 : 0);
        if (registry.get("rainbow")) {
            d += "add_button|rainbow|Rainbow skin on/off|noflags|0|0|\n";
        }
        d += "add_spacer|small|\n";

        d += "add_textbox|`wInfo``|left|\n";
        if (features.in_world()) {
            d += "add_button|players|Players in this world|noflags|0|0|\n";
            d += "add_button|world_info|World info|noflags|0|0|\n";
        }
        d += "add_text_input|find|Find item:||30|\n";
        d += "add_button|find_item|Search items|noflags|0|0|\n";
        d += "add_button|commands|All proxy commands|noflags|0|0|\n";
        d += "add_quick_exit|\n";
        d += fmt::format("end_dialog|{}|Close|Apply|\n", DIALOG_NAME);
        return d;
    }
};
}
