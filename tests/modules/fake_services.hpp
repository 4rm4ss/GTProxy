#pragma once
#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "modules/api.hpp"

// The proxy's side of the module API for tests: a world BOXTEST (12x9) with
// you (netID 7) at tile 5, 4, an inventory and one item (242, World Lock).
// Everything modules send is recorded.
namespace modules::testing {
class FakeServices final : public modules::Services {
public:
    [[nodiscard]] modules::WorldView world() const override
    {
        modules::WorldView view{};
        view.connected = true;
        view.in_world = true;
        view.net_id = 7;
        view.real_name = "`2Tester";
        view.previous = "START";
        view.name = "BOXTEST";
        view.width = 12;
        view.height = 9;
        view.me = modules::PlayerView{ 160.0f, 128.0f, 5, 4 };
        return view;
    }

    [[nodiscard]] modules::ReachView reach(const map::TilePos tile) override
    {
        reached.push_back(tile);
        modules::ReachView view{};
        view.x = tile.x;
        view.y = tile.y;
        view.status = tile.x == 0 ? "reachable" : "unreachable";
        view.message = tile.x == 0 ? "Цель доступна" : "Цель недоступна";
        view.start = map::TilePos{ 5, 4 };
        view.steps = 5;
        view.steps_word = "шагов";
        view.summary = "summary";
        return view;
    }

    bool show(const map::Card& card) override
    {
        cards.push_back(card);
        return true;
    }

    void reply(const modules::Source source, const std::string& module_id, const std::string& text) override
    {
        replies.push_back((source == modules::Source::Game ? "game:" : "console:") + module_id + ":" + text);
    }

    [[nodiscard]] std::vector<modules::PlayerInfo> players() const override
    {
        modules::PlayerInfo me{};
        me.net_id = 7;
        me.name = "`2Tester";
        me.local = true;
        me.position = modules::PlayerView{ 160.0f, 128.0f, 5, 4 };
        return { me };
    }

    [[nodiscard]] std::vector<modules::ObjectInfo> objects() const override { return { { 3, 242, 5, 64.0f, 96.0f } }; }

    [[nodiscard]] std::optional<modules::TileInfo> tile(const map::TilePos tile) const override
    {
        if (tile.x != 1 || tile.y != 1) {
            return std::nullopt;
        }
        return modules::TileInfo{ 10, 14, 0, "solid", "Tile 1, 1: Rock (10) - solid." };
    }

    [[nodiscard]] modules::InventoryView inventory() const override
    {
        return { true, 16, { { 2, 200, 0 }, { 242, 5, 0 }, { 1796, 1, 0 } } };
    }

    [[nodiscard]] bool items_loaded() const override { return true; }

    [[nodiscard]] std::optional<modules::ItemView> item(const std::uint32_t id) const override
    {
        if (id != 242) {
            return std::nullopt;
        }
        return modules::ItemView{ 242, "World Lock", "Lock", 1, 200, false };
    }

    [[nodiscard]] std::vector<modules::ItemView> find_items(const std::string_view query, std::size_t) const override
    {
        if (query == "lock") {
            return { *item(242) };
        }
        return {};
    }

    [[nodiscard]] char prefix() const override { return '/'; }

    void game_message(const std::string& text) override { messages.push_back(text); }
    void game_dialog(const std::string& markup) override { dialogs.push_back(markup); }
    bool game_call(const modules::CallOut& call) override
    {
        calls.push_back(call);
        return true;
    }
    bool server_text(const std::string& text, const bool game_message) override
    {
        sent.push_back((game_message ? "game:" : "generic:") + text);
        return true;
    }

    modules::WalkView walk_to(const map::TilePos tile, const std::optional<bool> gravity) override
    {
        walking_ = true;
        walk_gravity = gravity;
        return { true, "Walking", tile };
    }
    modules::WalkView walk_stop() override
    {
        walking_ = false;
        return { true, "Stopped", std::nullopt };
    }
    [[nodiscard]] bool walking() const override { return walking_; }
    [[nodiscard]] modules::WalkOptions walk_options() const override { return walk_options_; }
    modules::WalkOptions set_walk_options(const modules::WalkOptions& options) override
    {
        walk_options_ = options;
        walk_options_.speed = std::min(options.speed, 10000.0f);
        return walk_options_;
    }

    std::string save_setting(const std::string& module_id, const std::string& key, const modules::SettingValue& value) override
    {
        saved.push_back(module_id + ":" + key + "=" + modules::to_string(value));
        return {};
    }

    bool capture_wheel(const std::string& module_id, const std::string& mode) override
    {
        wheel_modes.push_back(module_id + ":" + mode);
        return true;
    }

    // API 3
    [[nodiscard]] std::vector<modules::VendingInfo> vendings() const override { return vendings_; }
    [[nodiscard]] std::vector<modules::LockInfo> locks() const override { return locks_; }
    [[nodiscard]] modules::WorldSummary world_summary() const override { return summary_; }
    [[nodiscard]] std::vector<modules::TileHit> find_tiles(const std::uint16_t item) const override
    {
        std::vector<modules::TileHit> hits;
        for (const auto& hit : tiles_) {
            if (hit.first == item) {
                hits.push_back(hit.second);
            }
        }
        return hits;
    }
    [[nodiscard]] modules::RouteView route(const map::TilePos tile, const bool walk, const std::optional<bool> gravity) override
    {
        routes.push_back({ tile, walk, gravity });
        if (tile.x == 99) {
            return { false, {}, "there is no way to get there" };
        }
        // A straight way from 5, 4 for tests.
        modules::RouteView view{ true, {}, {} };
        for (int x{ 5 }; x != tile.x; x += tile.x > 5 ? 1 : -1) {
            view.tiles.push_back({ x, 4 });
        }
        view.tiles.push_back(tile);
        return view;
    }
    [[nodiscard]] std::vector<int> distances() override { return distances_; }
    bool tile_action(const map::TilePos tile, const std::uint32_t item) override
    {
        actions.push_back(fmt::format("{},{}:{}", tile.x, tile.y, item));
        return true;
    }
    void set_overlay(const std::string& module_id, const std::string& id, const std::optional<modules::Overlay>& overlay) override
    {
        if (overlay) {
            overlays[module_id + "/" + id] = *overlay;
        }
        else {
            overlays.erase(module_id + "/" + id);
        }
    }
    void set_panel(const std::string& module_id, const std::string& id, const std::optional<std::string>& json) override
    {
        if (json) {
            panels[module_id + "/" + id] = *json;
        }
        else {
            panels.erase(module_id + "/" + id);
        }
    }
    void request_pick(const std::string& module_id, const std::string& label) override { picks.push_back(module_id + ":" + label); }
    std::string render_png(std::string& error) override
    {
        error.clear();
        return "data/renders/BOXTEST.png";
    }

    // API 4
    [[nodiscard]] modules::AbilitiesView abilities() const override
    {
        return { !official_, official_ ? "official" : "", false, false, abilities_ };
    }
    modules::AbilitiesView set_abilities(const modules::Abilities& abilities) override
    {
        if (!official_) {
            abilities_ = abilities;
        }
        ++ability_sets;
        return this->abilities();
    }
    [[nodiscard]] bool key_down(const std::string_view key) const override { return keys_down.contains(std::string{ key }); }
    [[nodiscard]] bool map_window() const override { return map_window_; }

    modules::Abilities abilities_{};
    bool official_{ false };
    int ability_sets{ 0 };
    std::set<std::string> keys_down;
    bool map_window_{ false };

    struct RouteAsk {
        map::TilePos tile;
        bool walk{ false };
        std::optional<bool> gravity;
    };

    modules::WalkOptions walk_options_{};
    std::vector<std::string> saved;
    std::vector<std::string> wheel_modes;
    std::optional<bool> walk_gravity;
    std::vector<modules::VendingInfo> vendings_;
    std::vector<modules::LockInfo> locks_;
    modules::WorldSummary summary_;
    std::vector<std::pair<std::uint16_t, modules::TileHit>> tiles_;
    std::vector<int> distances_;
    std::vector<RouteAsk> routes;
    std::vector<std::string> actions;
    std::map<std::string, modules::Overlay> overlays;
    std::map<std::string, std::string> panels;
    std::vector<std::string> picks;

    std::vector<map::TilePos> reached;
    std::vector<map::Card> cards;
    std::vector<std::string> replies;
    std::vector<std::string> messages;
    std::vector<std::string> dialogs;
    std::vector<modules::CallOut> calls;
    std::vector<std::string> sent;
    bool walking_{ false };
};

}
