#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "fake_services.hpp"
#include "modules/host.hpp"
#include "modules/manifest.hpp"

namespace fs = std::filesystem;
using modules::ModuleHost;
using modules::Source;

namespace {
using modules::testing::FakeServices;

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

    void add(const std::string& id, const std::string& code, const std::string& settings = "{}", const int api = 1) const
    {
        fs::create_directories(root_ / id);
        std::ofstream{ root_ / id / "module.json" } << R"({ "id": ")" << id
            << R"(", "version": "1.0.0", "api_version": )" << api << R"(, "main": "main.lua", "settings": )" << settings << " }";
        std::ofstream{ root_ / id / "main.lua" } << code;
    }

    void file(const std::string& path, const std::string& text) const
    {
        fs::create_directories((root_ / path).parent_path());
        std::ofstream{ root_ / path } << text;
    }

    [[nodiscard]] const fs::path& root() const { return root_; }

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
assert(gtproxy.api_version == 3)
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
    dir.add("c_event", R"(gtproxy.on("no_such_event", function() end))");
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

// ---- API 2 ----

TEST(ModuleHostTest, EventsGetTheirFieldsAndCanCancel)
{
    TempModules dir{};
    dir.add("a_walk", R"(
        gtproxy.on("tile_click", function(e)
            if e.x == 1 then return "cancel" end
        end)
        gtproxy.on("walk_end", function(e) gtproxy.game.message("walk: " .. e.message) end)
        gtproxy.on("inventory", function(e)
            if not e.full then gtproxy.game.message(string.format("inv %d -%d +%d", e.item, e.removed, e.added)) end
        end)
    )", "{}", 2);
    dir.add("b_broken", R"(gtproxy.on("tile_click", function() error("boom") end))", "{}", 2);
    FakeServices services{};
    ModuleHost host{ services };
    host.set_available(dir.manifests());
    host.apply(everything_on(host));
    ASSERT_TRUE(host.loaded("a_walk"));

    const auto tile_click = [&](const int x) {
        return host.emit("tile_click", [&](sol::table& event) {
            event["x"] = x;
            event["y"] = 2;
        });
    };
    auto outcome{ tile_click(1) };
    EXPECT_EQ(outcome.ran, 2u);
    EXPECT_TRUE(outcome.cancelled);
    outcome = tile_click(5);
    EXPECT_EQ(outcome.ran, 2u);
    EXPECT_FALSE(outcome.cancelled);     // a failing handler cancels nothing

    host.emit("walk_end", [](sol::table& event) { event["message"] = "Arrived."; });
    host.emit("inventory", [](sol::table& event) {
        event["full"] = false;
        event["item"] = 242;
        event["removed"] = 5;
        event["added"] = 0;
    });
    EXPECT_EQ(host.emit("disconnect").ran, 0u);
    EXPECT_EQ(services.messages, (std::vector<std::string>{ "walk: Arrived.", "inv 242 -5 +0" }));
}

TEST(ModuleHostTest, CallHooksChangeOrCancelServerCalls)
{
    TempModules dir{};
    dir.add("a_names", R"(
        gtproxy.hook_call("OnNameChanged", function(call)
            call.args[1] = call.args[1] .. " `4[" .. call.net_id .. "]``"
            return "replace"
        end)
        gtproxy.hook_call("OnRemove", function(call)
            call.args[1] = call.args[1] + 1
            return "replace"
        end)
        gtproxy.hook_call("OnSetPos", function(call)
            call.args[1].x = call.args[1].x + 32
            return "replace"
        end)
        gtproxy.hook_call("OnDialogRequest", function(call)
            if call.args[1]:find("drop_item", 1, true) then return "cancel" end
        end)
    )", "{}", 2);
    dir.add("b_never", R"(gtproxy.hook_call("OnDialogRequest", function() gtproxy.game.message("b saw it") end))", "{}", 2);
    dir.add("c_not_hookable", R"(gtproxy.hook_call("OnConsoleMessage", function() end))", "{}", 2);
    FakeServices services{};
    ModuleHost host{ services };
    host.set_available(dir.manifests());
    host.apply(everything_on(host));
    EXPECT_FALSE(host.loaded("c_not_hookable"));

    ModuleHost::Call name{ "OnNameChanged", 7, { std::string{ "`2Tester" }, std::string{ "{}" } } };
    auto outcome{ host.hook_call(name) };
    EXPECT_TRUE(outcome.replaced);
    EXPECT_EQ(std::get<std::string>(name.args[0]), "`2Tester `4[7]``");
    EXPECT_EQ(std::get<std::string>(name.args[1]), "{}");

    // Numbers keep the type the server used.
    ModuleHost::Call remove{ "OnRemove", -1, { std::uint32_t{ 5 } } };
    host.hook_call(remove);
    ASSERT_TRUE(std::holds_alternative<std::uint32_t>(remove.args[0]));
    EXPECT_EQ(std::get<std::uint32_t>(remove.args[0]), 6u);

    ModuleHost::Call pos{ "OnSetPos", 7, { glm::vec2{ 32.0f, 64.0f } } };
    host.hook_call(pos);
    EXPECT_EQ(std::get<glm::vec2>(pos.args[0]), (glm::vec2{ 64.0f, 64.0f }));

    // "cancel" ends it: later hooks do not see the call.
    ModuleHost::Call drop{ "OnDialogRequest", -1, { std::string{ "end_dialog|drop_item|Cancel|OK|" } } };
    outcome = host.hook_call(drop);
    EXPECT_TRUE(outcome.cancelled);
    EXPECT_EQ(outcome.ran, 1u);
    ModuleHost::Call other{ "OnDialogRequest", -1, { std::string{ "end_dialog|other|Cancel|OK|" } } };
    outcome = host.hook_call(other);
    EXPECT_FALSE(outcome.cancelled);
    EXPECT_FALSE(outcome.replaced);
    EXPECT_EQ(outcome.ran, 2u);
    EXPECT_EQ(services.messages, (std::vector<std::string>{ "b saw it" }));
}

TEST(ModuleHostTest, TimersRunFromUpdate)
{
    TempModules dir{};
    dir.add("timers", R"(
        local once, ticks = 0, 0
        gtproxy.after(10, function() once = once + 1 end)
        local cancelled = gtproxy.after(10, function() once = once + 100 end)
        assert(gtproxy.cancel(cancelled))
        local id
        id = gtproxy.every(10, function()
            ticks = ticks + 1
            if ticks == 3 then gtproxy.cancel(id) end
        end)
        gtproxy.every(10, function() error("broken timer") end)
        gtproxy.command("timers", "", function(args, ctx) ctx.reply(once .. " " .. ticks) end)
    )", "{}", 2);
    FakeServices services{};
    ModuleHost host{ services };
    host.set_available(dir.manifests());
    host.apply(everything_on(host));
    ASSERT_TRUE(host.loaded("timers"));

    for (int i{ 0 }; i < 20; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds{ 15 });
        host.update();
    }
    host.run_command("timers", {}, Source::Console);
    EXPECT_EQ(services.replies, (std::vector<std::string>{ "console:timers:1 3" }));
}

TEST(ModuleHostTest, StoreKeepsDataAcrossLoads)
{
    TempModules dir{};
    dir.add("keeper", R"(
        gtproxy.command("put", "", function(args, ctx)
            gtproxy.store.set("gap", tonumber(args[1]))
            gtproxy.store.set("fast", true)
            gtproxy.store.set("keep", "242,1796")
            gtproxy.store.set("gone", nil)
        end)
        gtproxy.command("get", "", function(args, ctx)
            ctx.reply(string.format("%s %s %s %s", tostring(gtproxy.store.get("gap", 200)), tostring(gtproxy.store.get("fast", false)),
                gtproxy.store.get("keep", ""), tostring(gtproxy.store.get("gone"))))
        end)
    )", "{}", 2);
    FakeServices services{};
    ModuleHost host{ services };
    host.set_data_dir(dir.root() / "data");
    host.set_available(dir.manifests());
    auto resolved{ everything_on(host) };
    host.apply(resolved);

    host.run_command("get", {}, Source::Console);
    host.run_command("put", { "350" }, Source::Console);

    // Off and on again: a new Lua state reads data/keeper.json.
    resolved.enabled["keeper"] = false;
    host.apply(resolved);
    resolved.enabled["keeper"] = true;
    host.apply(resolved);
    host.run_command("get", {}, Source::Console);
    EXPECT_EQ(services.replies, (std::vector<std::string>{ "console:keeper:200 false  nil", "console:keeper:350 true 242,1796 nil" }));
    EXPECT_TRUE(fs::exists(dir.root() / "data" / "keeper.json"));

    // A broken data file: the module starts without it, and the file is
    // kept as .broken when the module saves again.
    dir.file("data/keeper.json", "{ broken");
    resolved.enabled["keeper"] = false;
    host.apply(resolved);
    resolved.enabled["keeper"] = true;
    host.apply(resolved);
    ASSERT_TRUE(host.loaded("keeper"));
    host.run_command("put", { "400" }, Source::Console);
    EXPECT_TRUE(fs::exists(dir.root() / "data" / "keeper.json.broken"));
}

TEST(ModuleHostTest, LoadRunsOtherFilesOfTheModule)
{
    TempModules dir{};
    dir.add("multi", R"(
        local util = gtproxy.load("lib/util.lua")
        assert(util.answer == 42)
        assert(gtproxy.load("lib/util.lua") == util, "loaded once")
        assert(not pcall(gtproxy.load, "../other/main.lua"))
        assert(not pcall(gtproxy.load, "lib/missing.lua"))
        assert(not pcall(gtproxy.load, "data.txt"))
    )", "{}", 2);
    dir.file("multi/lib/util.lua", "return { answer = 42 }");
    FakeServices services{};
    ModuleHost host{ services };
    host.set_available(dir.manifests());
    host.apply(everything_on(host));
    EXPECT_TRUE(host.loaded("multi"));
}

TEST(ModuleHostTest, MenuSectionsAndStatusLines)
{
    TempModules dir{};
    dir.add("a_drop", R"(
        local busy = false
        gtproxy.menu.add({ title = "Items", icon = 242, button = "drop_page", label = "Inventory & drop", order = 10,
            status = function() if busy then return "`oDropping..." end end })
        gtproxy.command("busy", "", function() busy = true end)
    )", "{}", 2);
    dir.add("b_worlds", R"(
        gtproxy.menu.add({ title = "Worlds", icon = 3802, button = "worlds_page", label = "Warp", order = 20,
            status = function() return { "`oLast world: `wSTART``" } end })
        assert(not pcall(gtproxy.menu.add, { title = "no button" }))
    )", "{}", 2);
    FakeServices services{};
    ModuleHost host{ services };
    host.set_available(dir.manifests());
    host.apply(everything_on(host));

    const auto sections{ host.menu_sections() };
    ASSERT_EQ(sections.size(), 2u);
    EXPECT_EQ(sections[0].module, "a_drop");
    EXPECT_EQ(sections[0].title, "Items");
    EXPECT_EQ(sections[0].icon, 242);
    EXPECT_EQ(sections[0].button, "drop_page");
    EXPECT_EQ(sections[0].order, 10);
    EXPECT_EQ(sections[1].label, "Warp");

    EXPECT_EQ(host.menu_status(), (std::vector<std::string>{ "`oLast world: `wSTART``" }));
    host.run_command("busy", {}, Source::Game);
    EXPECT_EQ(host.menu_status(), (std::vector<std::string>{ "`oDropping...", "`oLast world: `wSTART``" }));
}

TEST(ModuleHostTest, TalkingToTheGameAndTheServer)
{
    TempModules dir{};
    dir.add("talk", R"(
        gtproxy.game.call("OnChangeSkin", 7, gtproxy.u32(0xFF0000FF))
        gtproxy.game.call("OnNameChanged", 7, "`2Nick", "{}")
        gtproxy.game.call("OnSetPos", 7, gtproxy.vec2(32, 64))
        gtproxy.game.call("OnCountdownStart", -1, 5, 1.5, gtproxy.f32(2), gtproxy.i32(-3))
        gtproxy.server.text("action|drop\n|itemID|2\n")
        gtproxy.server.game_text("action|join_request\nname|START\ninvitedWorld|0\n")
        gtproxy.game.message("`oHello")
        gtproxy.game.dialog(gtproxy.dialog()
            :header("Test", 242)
            :small("`oWorld: `wSTART``")
            :item("`wWorld Lock`` x5", 242)
            :input("count", "Count", "a|b", 5)
            :checkbox("fast", "Fast drop", true)
            :spacer()
            :button("go", "Go")
            :finish("gtproxy_test", "OK"))
    )", "{}", 2);
    FakeServices services{};
    ModuleHost host{ services };
    host.set_available(dir.manifests());
    host.apply(everything_on(host));
    ASSERT_TRUE(host.loaded("talk"));

    ASSERT_EQ(services.calls.size(), 4u);
    EXPECT_EQ(services.calls[0].name, "OnChangeSkin");
    EXPECT_EQ(services.calls[0].net_id, 7);
    EXPECT_EQ(std::get<std::uint32_t>(services.calls[0].args[0]), 0xFF0000FFu);
    EXPECT_EQ(std::get<std::string>(services.calls[1].args[0]), "`2Nick");
    EXPECT_EQ(std::get<glm::vec2>(services.calls[2].args[0]), (glm::vec2{ 32.0f, 64.0f }));
    EXPECT_EQ(std::get<std::int32_t>(services.calls[3].args[0]), 5);
    EXPECT_FLOAT_EQ(std::get<float>(services.calls[3].args[1]), 1.5f);
    EXPECT_FLOAT_EQ(std::get<float>(services.calls[3].args[2]), 2.0f);
    EXPECT_EQ(std::get<std::int32_t>(services.calls[3].args[3]), -3);

    EXPECT_EQ(services.sent, (std::vector<std::string>{
        "generic:action|drop\n|itemID|2\n",
        "game:action|join_request\nname|START\ninvitedWorld|0\n",
    }));
    EXPECT_EQ(services.messages, (std::vector<std::string>{ "`oHello" }));
    ASSERT_EQ(services.dialogs.size(), 1u);
    EXPECT_EQ(services.dialogs[0],
        "set_default_color|`o\n"
        "add_label_with_icon|big|`wTest``|left|242|\n"
        "add_smalltext|`oWorld: `wSTART``|\n"
        "add_label_with_icon|small|`wWorld Lock`` x5|left|242|\n"
        "add_text_input|count|Count|ab|5|\n"
        "add_checkbox|fast|Fast drop|1|\n"
        "add_spacer|small|\n"
        "add_button|go|Go|noflags|0|0|\n"
        "add_quick_exit|\n"
        "end_dialog|gtproxy_test|Close|OK|\n");
}

TEST(ModuleHostTest, ReadingTheWorldInventoryAndItems)
{
    TempModules dir{};
    dir.add("reader", R"(
        local w = gtproxy.world.info()
        assert(w.in_world and w.name == "BOXTEST" and w.width == 12 and w.me.tile_y == 4)
        local players = gtproxy.world.players()
        assert(#players == 1 and players[1].net_id == 7 and players[1].is_local and players[1].tile_x == 5)
        local objects = gtproxy.world.objects()
        assert(#objects == 1 and objects[1].item == 242 and objects[1].amount == 5)
        local tile = gtproxy.world.tile(1, 1)
        assert(tile.fg == 10 and tile.bg == 14 and tile.cell == "solid")
        assert(gtproxy.world.tile(2, 2) == nil)
        local inv = gtproxy.inventory.info()
        assert(inv.known and inv.slots == 16 and #inv.items == 3)
        assert(gtproxy.inventory.count(242) == 5 and gtproxy.inventory.count(9) == 0)
        assert(gtproxy.items.loaded())
        local lock = gtproxy.items.get(242)
        assert(lock.name == "World Lock" and lock.type == "Lock" and not lock.untradeable)
        assert(gtproxy.items.get(1) == nil)
        assert(#gtproxy.items.find("lock") == 1)
        assert(gtproxy.prefix == "/")
        assert(gtproxy.text.strip_colors("`2Hi``") == "Hi")
        assert(not gtproxy.walk.active())
        local walk = gtproxy.walk.to(3, 4)
        assert(walk.ok and walk.x == 3 and walk.y == 4 and gtproxy.walk.active())
        assert(gtproxy.walk.stop().ok and not gtproxy.walk.active())
    )", "{}", 2);
    FakeServices services{};
    ModuleHost host{ services };
    host.set_available(dir.manifests());
    host.apply(everything_on(host));
    EXPECT_TRUE(host.loaded("reader"));
}
