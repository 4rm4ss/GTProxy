#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "fake_services.hpp"
#include "modules/host.hpp"
#include "modules/manifest.hpp"

// The modules shipped in modules/ (the proxy's own features), loaded the way
// the proxy loads them, against modules::testing::FakeServices.
namespace fs = std::filesystem;
using modules::ModuleHost;
using modules::Source;
using modules::testing::FakeServices;

namespace {
const fs::path SOURCE{ GTPROXY_SOURCE_DIR };

class TempDir {
public:
    TempDir()
        : path_{ fs::temp_directory_path() / ("gtproxy-shipped-" + std::to_string(std::random_device{}())) }
    {
        fs::create_directories(path_);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    [[nodiscard]] const fs::path& path() const { return path_; }

private:
    fs::path path_;
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

// The proxy's own commands (the core keeps them).
bool reserved(const std::string_view name)
{
    return name == "proxy" || name == "proxyhelp" || name == "map";
}

class Shipped : public ::testing::Test {
protected:
    void SetUp() override
    {
        host.set_data_dir(data.path());
        host.set_reserved_names(reserved);
        auto found{ modules::discover(SOURCE / "modules") };
        problems = found.problems;
        host.set_available(std::move(found.modules));
        host.apply(everything_on(host));
    }

    // Runs the module timers for a while.
    void run_for(const std::chrono::milliseconds duration)
    {
        const auto end{ std::chrono::steady_clock::now() + duration };
        while (std::chrono::steady_clock::now() < end) {
            std::this_thread::sleep_for(std::chrono::milliseconds{ 5 });
            host.update();
        }
    }

    [[nodiscard]] bool replied(const std::string_view part) const
    {
        return std::ranges::any_of(services.replies, [&](const std::string& line) { return line.find(part) != std::string::npos; });
    }

    TempDir data{};
    FakeServices services{};
    ModuleHost host{ services };
    std::vector<std::string> problems;
};
}

TEST_F(Shipped, AllModulesLoad)
{
    EXPECT_TRUE(problems.empty());
    for (const auto* id : { "autohost", "camera", "drop", "info", "item_finder", "looks", "reachability", "vend_finder", "walk",
             "world_moderation", "world_scan", "worlds" }) {
        EXPECT_TRUE(host.loaded(id)) << id;
    }

    std::set<std::string> names;
    for (const auto& command : host.commands()) {
        names.insert(command.name);
    }
    const std::set<std::string> expected{
        "back", "balance", "clickwalk", "daw", "db", "dd", "ddl", "debug", "drop", "dropall", "dw", "exit", "fastdrop",
        "fasttrash", "find", "goto", "inv", "item", "keep", "netid", "nick", "players", "pos", "rainbow", "reach",
        "rejoin", "skin", "walking", "warp", "world", "zoom",
        // the five modules of the local test world
        "pvend", "pfind", "growscan", "track", "render", "pos1", "pos2", "pos3", "pos4", "host", "win1", "win2", "win3",
        "win4", "wm",
    };
    EXPECT_EQ(names, expected);

    auto sections{ host.menu_sections() };
    std::ranges::stable_sort(sections, {}, &ModuleHost::MenuSection::order);
    std::vector<std::string> buttons;
    for (const auto& section : sections) {
        buttons.push_back(section.button);
    }
    EXPECT_EQ(buttons, (std::vector<std::string>{ "page_worlds", "page_walking", "page_items", "page_looks", "page_camera", "page_info" }));
}

TEST_F(Shipped, DropModuleItemHelpers)
{
    // tests/modules/lua/test_drop_items.lua as a module next to a copy of
    // modules/drop/items.lua: it loads only if every check passes.
    TempDir dir{};
    fs::create_directories(dir.path() / "drop_items_test");
    std::ofstream{ dir.path() / "drop_items_test" / "module.json" }
        << R"({ "id": "drop_items_test", "version": "1.0.0", "api_version": 2, "main": "test_drop_items.lua" })";
    fs::copy_file(SOURCE / "tests/modules/lua/test_drop_items.lua", dir.path() / "drop_items_test" / "test_drop_items.lua");
    fs::copy_file(SOURCE / "modules/drop/items.lua", dir.path() / "drop_items_test" / "items.lua");

    FakeServices fake{};
    ModuleHost test{ fake };
    test.set_available(modules::discover(dir.path()).modules);
    test.apply(everything_on(test));
    EXPECT_TRUE(test.loaded("drop_items_test")) << "a check failed: see the log above";
}

// tests/modules/lua/<test> as a module next to a copy of a module's helper:
// it loads only if every check in it passes.
bool lua_helper_tests_pass(const std::string& test, const std::string& helper, const std::string& as)
{
    TempDir dir{};
    const auto folder{ dir.path() / "helper_test" };
    fs::create_directories(folder);
    std::ofstream{ folder / "module.json" }
        << R"({ "id": "helper_test", "version": "1.0.0", "api_version": 3, "main": "test.lua" })";
    fs::copy_file(SOURCE / "tests/modules/lua" / test, folder / "test.lua");
    fs::copy_file(SOURCE / "modules" / helper, folder / as);

    FakeServices fake{};
    ModuleHost test_host{ fake };
    test_host.set_available(modules::discover(dir.path()).modules);
    test_host.apply(everything_on(test_host));
    return test_host.loaded("helper_test");
}

TEST(ShippedHelpers, VendingPrices)
{
    EXPECT_TRUE(lua_helper_tests_pass("test_vend_price.lua", "vend_finder/price.lua", "price.lua")) << "a check failed: see the log above";
}

TEST(ShippedHelpers, ItemFinderChoice)
{
    EXPECT_TRUE(lua_helper_tests_pass("test_item_find.lua", "item_finder/find.lua", "find.lua")) << "a check failed: see the log above";
}

TEST(ShippedHelpers, WorldScanCounts)
{
    EXPECT_TRUE(lua_helper_tests_pass("test_world_scan.lua", "world_scan/scan.lua", "scan.lua")) << "a check failed: see the log above";
}

TEST(ShippedHelpers, ModerationRules)
{
    EXPECT_TRUE(lua_helper_tests_pass("test_moderation_rules.lua", "world_moderation/rules.lua", "rules.lua")) << "a check failed: see the log above";
}

TEST(ShippedHelpers, AutohostPayout)
{
    EXPECT_TRUE(lua_helper_tests_pass("test_autohost_payout.lua", "autohost/payout.lua", "payout.lua")) << "a check failed: see the log above";
}

TEST_F(Shipped, DropAnswersTheServersDialogs)
{
    // 1 Diamond Lock + 5 World Locks (FakeServices): 300 WL cannot be paid.
    host.run_command("dd", { "300" }, Source::Game);
    EXPECT_TRUE(replied("Can't make exactly `w300 WL``"));
    run_for(std::chrono::milliseconds{ 50 });
    EXPECT_TRUE(services.sent.empty());

    host.run_command("dd", { "105" }, Source::Game);
    EXPECT_TRUE(replied("105 WL = `w1`` Diamond Lock + `w5`` World Lock"));
    run_for(std::chrono::milliseconds{ 50 });
    ASSERT_EQ(services.sent, (std::vector<std::string>{ "generic:action|drop\n|itemID|1796\n" }));

    // The server asks "how many?": answered with 1, the dialog never shows.
    ModuleHost::Call dialog{ "OnDialogRequest", -1, { std::string{
        "add_label_with_icon|big|`wDrop Diamond Lock``|left|1796|\nadd_text_input|count||1|5|\nembed_data|itemID|1796\nend_dialog|drop_item|Cancel|OK|\n"
    } } };
    EXPECT_TRUE(host.hook_call(dialog).cancelled);
    ASSERT_EQ(services.sent.size(), 2u);
    EXPECT_EQ(services.sent[1], "generic:action|dialog_return\ndialog_name|drop_item\nitemID|1796|\ncount|1\n");

    // The server confirms: the World Locks follow after the pause.
    host.emit("inventory", [](sol::table& event) {
        event["full"] = false;
        event["item"] = 1796;
        event["removed"] = 1;
        event["added"] = 0;
    });
    run_for(std::chrono::milliseconds{ 400 });
    ASSERT_EQ(services.sent.size(), 3u);
    EXPECT_EQ(services.sent[2], "generic:action|drop\n|itemID|242\n");

    // Leaving the world stops the rest.
    host.emit("world_menu");
    EXPECT_TRUE(std::ranges::any_of(services.messages, [](const std::string& text) { return text.find("Drop stopped") != std::string::npos; }));

    // Other dialogs are left alone; fast drop answers your own drop dialog.
    ModuleHost::Call other{ "OnDialogRequest", -1, { std::string{ "add_button|x|X|noflags|0|0|\nend_dialog|popup|Close||\n" } } };
    EXPECT_FALSE(host.hook_call(other).cancelled);
    host.run_command("fastdrop", {}, Source::Game);
    ModuleHost::Call own{ "OnDialogRequest", -1, { std::string{ "add_text_input|count||200|5|\nembed_data|itemID|2\nend_dialog|drop_item|Cancel|OK|\n" } } };
    EXPECT_TRUE(host.hook_call(own).cancelled);
    EXPECT_EQ(services.sent.back(), "generic:action|dialog_return\ndialog_name|drop_item\nitemID|2|\ncount|200\n");
    EXPECT_TRUE(fs::exists(data.path() / "drop.json"));
}

TEST_F(Shipped, WarpExitsFirstThenJoins)
{
    host.run_command("warp", { "start", "shop_1" }, Source::Game);
    ASSERT_EQ(services.sent, (std::vector<std::string>{ "game:action|quit_to_exit" }));
    EXPECT_TRUE(replied("Warping to `wSTART|SHOP_1``"));
    host.emit("world_menu");
    ASSERT_EQ(services.sent.size(), 2u);
    EXPECT_EQ(services.sent[1], "game:action|join_request\nname|START|SHOP_1\ninvitedWorld|0");

    host.run_command("warp", { "bad-name" }, Source::Game);
    EXPECT_TRUE(replied("World names are 1-23 letters or digits"));
    EXPECT_EQ(services.sent.size(), 2u);
}

TEST_F(Shipped, NickAndNetIdChangeNamesInTheGame)
{
    ModuleHost::Call spawn{ "OnSpawn", -1, { std::string{
        "spawn|avatar\nnetID|7\nname|`wE2EPlayer``\ntitleIcon|{\"PlayerWorldID\":7}\ntype|local"
    } } };
    EXPECT_FALSE(host.hook_call(spawn).replaced);     // nothing on: unchanged

    host.run_command("nick", { "Tester" }, Source::Game);
    ASSERT_EQ(services.calls.size(), 1u);
    EXPECT_EQ(services.calls[0].name, "OnNameChanged");
    EXPECT_EQ(services.calls[0].net_id, 7);
    EXPECT_EQ(std::get<std::string>(services.calls[0].args[0]), "`wTester``");
    EXPECT_EQ(std::get<std::string>(services.calls[0].args[1]), "{\"PlayerWorldID\":7}");

    // Spawning again shows the nick; only the name line changes.
    EXPECT_TRUE(host.hook_call(spawn).replaced);
    EXPECT_EQ(std::get<std::string>(spawn.args[0]), "spawn|avatar\nnetID|7\nname|`wTester``\ntitleIcon|{\"PlayerWorldID\":7}\ntype|local");

    host.run_command("netid", {}, Source::Game);
    ASSERT_EQ(services.calls.size(), 2u);
    EXPECT_EQ(std::get<std::string>(services.calls[1].args[0]), "`wTester`` `4[7]``");
}

TEST_F(Shipped, WalkModuleAnswersClicks)
{
    EXPECT_EQ(host.emit("left_click", [](sol::table& event) {
        event["x"] = 3;
        event["y"] = 4;
    }).ran, 1u);
    EXPECT_TRUE(services.walking());

    // Click-walk off: punches pass; on: a punch becomes a walk.
    const auto punch = [&] {
        return host.emit("tile_click", [](sol::table& event) {
            event["x"] = 2;
            event["y"] = 2;
        });
    };
    EXPECT_FALSE(punch().cancelled);
    host.run_command("clickwalk", {}, Source::Game);
    EXPECT_TRUE(punch().cancelled);
    EXPECT_TRUE(std::ranges::any_of(services.messages, [](const std::string& text) { return text == "`o[map] Walking"; }));

    host.emit("walk_end", [](sol::table& event) {
        event["message"] = "Arrived at 2, 2.";
        event["notify"] = true;
    });
    EXPECT_EQ(services.messages.back(), "`o[map] Arrived at 2, 2.");
}

TEST_F(Shipped, CameraZoomsWithTheGamesOwnCall)
{
    host.run_command("zoom", { "0.25" }, Source::Game);
    ASSERT_EQ(services.calls.size(), 1u);
    EXPECT_EQ(services.calls[0].name, "OnZoomCamera");
    EXPECT_FLOAT_EQ(std::get<float>(services.calls[0].args[0]), 0.25f);
    EXPECT_EQ(std::get<std::int32_t>(services.calls[0].args[1]), 300);
    EXPECT_TRUE(replied("Camera zoom `w0.25`` (kept when you enter a world)"));

    // Entering a world resets the game's camera: the value is sent again.
    host.emit("world_enter", [](sol::table& event) { event["name"] = "START"; });
    run_for(std::chrono::milliseconds{ 1200 });
    ASSERT_EQ(services.calls.size(), 2u);
    EXPECT_FLOAT_EQ(std::get<float>(services.calls[1].args[0]), 0.25f);

    host.run_command("zoom", { "reset" }, Source::Game);
    EXPECT_FLOAT_EQ(std::get<float>(services.calls.back().args[0]), 1.0f);
    host.emit("world_enter", [](sol::table& event) { event["name"] = "START"; });
    run_for(std::chrono::milliseconds{ 1200 });
    EXPECT_EQ(services.calls.size(), 3u);      // not kept any more

    host.run_command("zoom", { "-1" }, Source::Game);
    EXPECT_TRUE(replied("Usage"));
    EXPECT_EQ(services.calls.size(), 3u);
}

TEST_F(Shipped, CameraTakesTheWheelAndZoomsWithoutLimits)
{
    // The wheel stays the game's unless asked for ("wheel": "off" by default).
    ASSERT_FALSE(services.wheel_modes.empty());
    EXPECT_EQ(services.wheel_modes.front(), "camera:off");
    host.run_command("zoom", { "wheel", "always" }, Source::Game);
    EXPECT_EQ(services.saved.back(), "camera:wheel=\"always\"");

    // Two notches away from you: closer by 1.15 twice; far beyond the game's
    // own range towards you.
    const auto wheel = [&](const int notches) {
        host.emit("wheel", [&](sol::table& event) { event["notches"] = notches; });
        return std::get<float>(services.calls.back().args[0]);
    };
    EXPECT_NEAR(wheel(2), 0.5f * 1.15f * 1.15f, 1e-4);
    EXPECT_NEAR(wheel(-40), 0.01f, 1e-6);       // stops at min_zoom
    EXPECT_EQ(std::get<std::int32_t>(services.calls.back().args[1]), 0);
}

TEST_F(Shipped, WalkingSettingsGoToTheEngineAndToTheConfig)
{
    // At load: the settings of config/modules/walk.json (module defaults here).
    EXPECT_FLOAT_EQ(services.walk_options_.speed, 250.0f);
    EXPECT_EQ(services.walk_options_.doors, "access");
    EXPECT_EQ(services.walk_options_.air_jumps, 1);

    // The Walking page's Save: changed values are written like a hand edit.
    host.emit("dialog", [](sol::table& event) {
        sol::state_view lua{ event.lua_state() };
        event["name"] = "gtproxy_walking";
        event["button"] = "walk_save";
        event["frame"] = false;
        event["values"] = lua.create_table_with(
            "speed", "900", "step_ms", "50", "gravity", "1", "jump_height", "4", "air_jumps", "1000"
        );
    });
    std::ranges::sort(services.saved);
    EXPECT_EQ(services.saved, (std::vector<std::string>{
        "walk:air_jumps=1000", "walk:gravity=true", "walk:jump_height=4", "walk:speed=900", "walk:step_ms=50",
    }));
    EXPECT_TRUE(std::ranges::any_of(services.dialogs, [](const std::string& d) { return d.find("end_dialog|gtproxy_walking|") != std::string::npos; }));

    services.saved.clear();
    host.emit("dialog", [](sol::table& event) {
        sol::state_view lua{ event.lua_state() };
        event["name"] = "gtproxy_walking";
        event["button"] = "walk_doors_all";
        event["frame"] = false;
        event["values"] = lua.create_table();
    });
    EXPECT_EQ(services.saved, (std::vector<std::string>{ "walk:doors=\"all\"" }));
}

TEST_F(Shipped, CameraZoomTestStepsThroughValues)
{
    host.run_command("zoom", { "test" }, Source::Game);
    ASSERT_EQ(services.calls.size(), 1u);
    EXPECT_FLOAT_EQ(std::get<float>(services.calls[0].args[0]), 0.5f);
    EXPECT_NE(services.messages.back().find("Zoom test `w1/7``"), std::string::npos);
    run_for(std::chrono::milliseconds{ 3200 });
    ASSERT_EQ(services.calls.size(), 2u);
    EXPECT_FLOAT_EQ(std::get<float>(services.calls[1].args[0]), 2.0f);
}
