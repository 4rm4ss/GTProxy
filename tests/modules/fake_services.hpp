#pragma once
#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

    modules::WalkView walk_to(const map::TilePos tile) override
    {
        walking_ = true;
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

    modules::WalkOptions walk_options_{};
    std::vector<std::string> saved;
    std::vector<std::string> wheel_modes;

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
