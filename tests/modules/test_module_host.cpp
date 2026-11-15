#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "modules/host.hpp"
#include "modules/manifest.hpp"

namespace fs = std::filesystem;
using modules::ModuleHost;
using modules::Source;

namespace {
class FakeServices final : public modules::Services {
public:
    [[nodiscard]] modules::WorldView world() const override
    {
        modules::WorldView view{};
        view.in_world = true;
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

    void reply(const Source source, const std::string& module_id, const std::string& text) override
    {
        replies.push_back((source == Source::Game ? "game:" : "console:") + module_id + ":" + text);
    }

    std::vector<map::TilePos> reached;
    std::vector<map::Card> cards;
    std::vector<std::string> replies;
};

class TempModules {
public:
    TempModules()
        : root_{ fs::temp_directory_path() / ("gtproxy-host-" + std::to_string(std::random_device{}())) }
    {
        fs::create_directories(root_);
    }
    ~TempModules()
    {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    void add(const std::string& id, const std::string& code, const std::string& settings = "{}") const
    {
        fs::create_directories(root_ / id);
        std::ofstream{ root_ / id / "module.json" } << R"({ "id": ")" << id
            << R"(", "version": "1.0.0", "api_version": 1, "main": "main.lua", "settings": )" << settings << " }";
        std::ofstream{ root_ / id / "main.lua" } << code;
    }

    [[nodiscard]] std::vector<modules::Manifest> manifests() const
    {
        auto found{ modules::discover(root_) };
        return std::move(found.modules);
    }

private:
    fs::path root_;
};

modules::Resolved everything_on(const ModuleHost& host)
{
    modules::Resolved resolved{};
    for (const auto& manifest : host.available()) {
        resolved.enabled[manifest.id] = true;
        resolved.settings[manifest.id] = manifest.settings;
    }
    return resolved;
}

const std::string REACH_MODULE{ R"(
gtproxy.on("right_click", function(e)
    local r = gtproxy.reach(e.x, e.y)
    gtproxy.show({ title = r.message, text = "steps " .. r.steps, kind = r.status == "reachable" and "ok" or "bad",
        marker = gtproxy.setting("show_marker"), status = r.status })
end)
gtproxy.command("reach", "check a tile", function(args, ctx)
    local r = gtproxy.reach(tonumber(args[1]), tonumber(args[2]))
    ctx.reply(ctx.source .. " " .. r.message)
end)
assert(dofile == nil and loadfile == nil, "no file access")
assert(gtproxy.api_version == 1)
assert(gtproxy.module.id == "reach")
assert(gtproxy.world.info().me.tile_x == 5)
)" };
}

TEST(ModuleHostTest, RightClickAndCommandsReachTheModule)
{
    TempModules dir{};
    dir.add("reach", REACH_MODULE, R"({ "show_marker": true })");
    FakeServices services{};
    ModuleHost host{ services };
    host.set_available(dir.manifests());
    host.apply(everything_on(host));
    ASSERT_TRUE(host.loaded("reach"));

    EXPECT_EQ(host.right_click({ 0, 4 }, true), 1u);
    ASSERT_EQ(services.cards.size(), 1u);
    EXPECT_EQ(services.cards[0].title, "Цель доступна");
    EXPECT_EQ(services.cards[0].text, "steps 5");
    EXPECT_EQ(services.cards[0].kind, "ok");
    EXPECT_TRUE(services.cards[0].marker);
    EXPECT_EQ(services.cards[0].status, "reachable");

    ASSERT_EQ(host.commands().size(), 1u);
    EXPECT_EQ(host.commands()[0].name, "reach");
    EXPECT_TRUE(host.run_command("reach", { "7", "1" }, Source::Console));
    EXPECT_TRUE(host.run_command("reach", { "0", "4" }, Source::Game));
    EXPECT_EQ(services.replies, (std::vector<std::string>{ "console:reach:console Цель недоступна", "game:reach:game Цель доступна" }));
    EXPECT_FALSE(host.run_command("nothing", {}, Source::Console));
}

TEST(ModuleHostTest, NewSettingsWithoutReloading)
{
    TempModules dir{};
    dir.add("reach", REACH_MODULE, R"({ "show_marker": true })");
    FakeServices services{};
    ModuleHost host{ services };
    host.set_available(dir.manifests());
    auto resolved{ everything_on(host) };
    host.apply(resolved);

    resolved.settings["reach"]["show_marker"] = false;
    host.apply(resolved);
    host.right_click({ 1, 1 }, true);
    ASSERT_EQ(services.cards.size(), 1u);
    EXPECT_FALSE(services.cards[0].marker);
}

TEST(ModuleHostTest, SwitchedOffModuleStopsAnswering)
{
    TempModules dir{};
    dir.add("reach", REACH_MODULE, R"({ "show_marker": true })");
    FakeServices services{};
    ModuleHost host{ services };
    int changes{ 0 };
    host.set_on_change([&] { ++changes; });
    host.set_available(dir.manifests());

    auto resolved{ everything_on(host) };
    host.apply(resolved);
    EXPECT_EQ(changes, 1);

    resolved.enabled["reach"] = false;
    host.apply(resolved);
    EXPECT_EQ(changes, 2);
    EXPECT_FALSE(host.loaded("reach"));
    EXPECT_EQ(host.right_click({ 0, 4 }, true), 0u);
    EXPECT_TRUE(host.commands().empty());
    EXPECT_FALSE(host.run_command("reach", { "0", "4" }, Source::Console));
    EXPECT_TRUE(services.cards.empty());

    // And back on.
    resolved.enabled["reach"] = true;
    host.apply(resolved);
    EXPECT_TRUE(host.loaded("reach"));
    EXPECT_EQ(host.right_click({ 0, 4 }, true), 1u);
}

TEST(ModuleHostTest, BrokenModulesDoNotStopTheOthers)
{
    TempModules dir{};
    dir.add("a_syntax", "this is not lua (");
    dir.add("b_crash", R"(gtproxy.on("right_click", function(e) error("boom") end))");
    dir.add("c_event", R"(gtproxy.on("left_click", function() end))");
    dir.add("reach", REACH_MODULE, R"({ "show_marker": true })");
    FakeServices services{};
    ModuleHost host{ services };
    host.set_available(dir.manifests());
    host.apply(everything_on(host));

    EXPECT_FALSE(host.loaded("a_syntax"));
    EXPECT_FALSE(host.loaded("c_event"));      // unknown event: load fails
    EXPECT_TRUE(host.loaded("b_crash"));
    EXPECT_TRUE(host.loaded("reach"));

    // b_crash's handler fails, reach's still answers.
    EXPECT_EQ(host.right_click({ 0, 0 }, true), 2u);
    EXPECT_EQ(services.cards.size(), 1u);
}

TEST(ModuleHostTest, EndlessLoopIsStopped)
{
    TempModules dir{};
    dir.add("loop", R"(gtproxy.on("right_click", function(e) while true do end end))");
    dir.add("reach", REACH_MODULE, R"({ "show_marker": true })");
    FakeServices services{};
    ModuleHost host{ services };
    host.set_available(dir.manifests());
    host.apply(everything_on(host));

    const auto start{ std::chrono::steady_clock::now() };
    EXPECT_EQ(host.right_click({ 0, 0 }, true), 2u);
    const auto took{ std::chrono::steady_clock::now() - start };
    EXPECT_LT(took, ModuleHost::TIME_LIMIT + std::chrono::seconds{ 2 });
    EXPECT_EQ(services.cards.size(), 1u);       // reach still answered
}

TEST(ModuleHostTest, CommandNamesAreChecked)
{
    TempModules dir{};
    dir.add("a_first", R"(gtproxy.command("reach", "first", function() end))");
    dir.add("b_second", R"(
        assert(gtproxy.command("reach", "second", function() end) == false)
        assert(gtproxy.command("warp", "taken by the proxy", function() end) == false)
        assert(gtproxy.command("own", "fine", function() end) == true)
    )");
    dir.add("c_bad_name", R"(gtproxy.command("Bad Name", "x", function() end))");
    FakeServices services{};
    ModuleHost host{ services };
    host.set_reserved_names([](const std::string_view name) { return name == "warp"; });
    host.set_available(dir.manifests());
    host.apply(everything_on(host));

    EXPECT_TRUE(host.loaded("a_first"));
    EXPECT_TRUE(host.loaded("b_second"));
    EXPECT_FALSE(host.loaded("c_bad_name"));
    std::vector<std::string> names;
    for (const auto& command : host.commands()) {
        names.push_back(command.module + "/" + command.name);
    }
    EXPECT_EQ(names, (std::vector<std::string>{ "a_first/reach", "b_second/own" }));
}

TEST(ModuleHostTest, SettingThatDoesNotExistIsAnError)
{
    TempModules dir{};
    dir.add("typo", R"(gtproxy.on("right_click", function(e) gtproxy.show({ title = tostring(gtproxy.setting("nope")) }) end))",
        R"({ "real": 1 })");
    FakeServices services{};
    ModuleHost host{ services };
    host.set_available(dir.manifests());
    host.apply(everything_on(host));
    EXPECT_EQ(host.right_click({ 0, 0 }, true), 1u);
    EXPECT_TRUE(services.cards.empty());
}
