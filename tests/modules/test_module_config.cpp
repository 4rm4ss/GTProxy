#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include <gtest/gtest.h>

#include "modules/manifest.hpp"
#include "modules/settings.hpp"

namespace fs = std::filesystem;
using modules::Settings;

namespace {
class TempDir {
public:
    TempDir()
        : path_{ fs::temp_directory_path() / ("gtproxy-modules-" + std::to_string(std::random_device{}())) }
    {
        fs::create_directories(path_);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    [[nodiscard]] const fs::path& path() const { return path_; }

    void write(const std::string& relative, const std::string& text) const
    {
        const auto file{ path_ / relative };
        fs::create_directories(file.parent_path());
        std::ofstream{ file, std::ios::binary } << text;
    }

private:
    fs::path path_;
};

const std::map<std::string, Settings> DEFAULTS{
    { "reachability", Settings{ { "show_marker", true }, { "show_steps", true }, { "log_result", true } } },
};

bool flag(const modules::Resolved& resolved, const std::string& key)
{
    return std::get<bool>(resolved.settings.at("reachability").at(key));
}

std::string all_reasons(const std::vector<modules::ConfigError>& errors)
{
    std::string text;
    for (const auto& error : errors) {
        text += error.file + ": " + error.reason + "\n";
    }
    return text;
}
}

TEST(ModuleConfigTest, NoConfigAtAllGivesDefaults)
{
    TempDir dir{};
    const modules::ConfigDir config{ dir.path() / "config" };
    const auto resolved{ config.resolve("default", DEFAULTS) };
    ASSERT_TRUE(resolved) << all_reasons(resolved.error());
    EXPECT_TRUE(resolved->enabled.at("reachability"));
    EXPECT_TRUE(flag(*resolved, "show_steps"));
    EXPECT_EQ(resolved->profile, "default");
}

TEST(ModuleConfigTest, DefaultsThenModuleFileThenProfile)
{
    TempDir dir{};
    dir.write("config/modules/reachability.json", R"({ "show_steps": false, "show_marker": false })");
    dir.write("config/profiles/default.json", R"({ "modules": {} })");
    dir.write("config/profiles/test.json", R"({ "description": "Test", "modules": { "reachability": { "show_steps": true } } })");
    const modules::ConfigDir config{ dir.path() / "config" };

    const auto by_default{ config.resolve("default", DEFAULTS) };
    ASSERT_TRUE(by_default) << all_reasons(by_default.error());
    EXPECT_FALSE(flag(*by_default, "show_steps"));    // module file over the default
    EXPECT_FALSE(flag(*by_default, "show_marker"));
    EXPECT_TRUE(flag(*by_default, "log_result"));     // default

    const auto test{ config.resolve("test", DEFAULTS) };
    ASSERT_TRUE(test) << all_reasons(test.error());
    EXPECT_TRUE(flag(*test, "show_steps"));           // profile over the module file
    EXPECT_FALSE(flag(*test, "show_marker"));         // module file, profile does not touch it

    // Back to default: nothing of "test" is left.
    const auto again{ config.resolve("default", DEFAULTS) };
    ASSERT_TRUE(again);
    EXPECT_FALSE(flag(*again, "show_steps"));
}

TEST(ModuleConfigTest, ModulesFileSwitchesModules)
{
    TempDir dir{};
    dir.write("config/modules.json", R"({ "profile": "test", "modules": { "reachability": { "enabled": false } } })");
    const modules::ConfigDir config{ dir.path() / "config" };

    const auto file{ config.read_modules_file() };
    ASSERT_TRUE(file);
    EXPECT_EQ(file->profile, "test");

    const auto resolved{ config.resolve("default", DEFAULTS) };
    ASSERT_TRUE(resolved);
    EXPECT_FALSE(resolved->enabled.at("reachability"));
}

TEST(ModuleConfigTest, BrokenJsonNamesFileAndPlace)
{
    TempDir dir{};
    dir.write("config/profiles/broken.json", "{\n  \"modules\": { \"reachability\": { \"show_steps\": fals } }\n}");
    const modules::ConfigDir config{ dir.path() / "config" };

    const auto resolved{ config.resolve("broken", DEFAULTS) };
    ASSERT_FALSE(resolved);
    ASSERT_EQ(resolved.error().size(), 1u);
    EXPECT_EQ(resolved.error()[0].file, "config/profiles/broken.json");
    EXPECT_NE(resolved.error()[0].reason.find("line 2"), std::string::npos) << resolved.error()[0].reason;
    EXPECT_NE(resolved.error()[0].reason.find("expected_true_or_false"), std::string::npos) << resolved.error()[0].reason;
}

TEST(ModuleConfigTest, WrongTypeAndUnknownSetting)
{
    TempDir dir{};
    dir.write("config/modules/reachability.json", R"({ "show_steps": "yes", "show_stepz": true })");
    const modules::ConfigDir config{ dir.path() / "config" };

    const auto resolved{ config.resolve("default", DEFAULTS) };
    ASSERT_FALSE(resolved);
    const auto text{ all_reasons(resolved.error()) };
    EXPECT_NE(text.find("config/modules/reachability.json: show_steps: must be true or false, not text (\"yes\")"), std::string::npos) << text;
    EXPECT_NE(text.find("show_stepz: no such setting (known: log_result, show_marker, show_steps)"), std::string::npos) << text;
}

TEST(ModuleConfigTest, ProfileErrors)
{
    TempDir dir{};
    dir.write("config/profiles/types.json", R"({ "modules": { "reachability": { "show_marker": 1 } } })");
    dir.write("config/profiles/nested.json", R"({ "modules": { "reachability": { "show_marker": { "a": 1 } } } })");
    dir.write("config/profiles/typo.json", R"({ "modulez": {} })");
    const modules::ConfigDir config{ dir.path() / "config" };

    const auto types{ config.resolve("types", DEFAULTS) };
    ASSERT_FALSE(types);
    EXPECT_NE(all_reasons(types.error()).find("modules.reachability.show_marker: must be true or false, not a number (1)"), std::string::npos)
        << all_reasons(types.error());

    const auto nested{ config.resolve("nested", DEFAULTS) };
    ASSERT_FALSE(nested);
    EXPECT_NE(all_reasons(nested.error()).find("true/false, a number or text"), std::string::npos) << all_reasons(nested.error());

    const auto typo{ config.resolve("typo", DEFAULTS) };
    ASSERT_FALSE(typo);
    EXPECT_NE(all_reasons(typo.error()).find("unknown_key"), std::string::npos) << all_reasons(typo.error());

    const auto missing{ config.resolve("nothere", DEFAULTS) };
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error()[0].file, "config/profiles/nothere.json");

    const auto path{ config.resolve("../modules", DEFAULTS) };
    ASSERT_FALSE(path);
    EXPECT_NE(all_reasons(path.error()).find("not a valid profile name"), std::string::npos);
}

TEST(ModuleConfigTest, NotepadBomAndEmptyFile)
{
    TempDir dir{};
    dir.write("config/profiles/bom.json", "\xEF\xBB\xBF{ \"modules\": { \"reachability\": { \"log_result\": false } } }");
    dir.write("config/profiles/empty.json", "  \n");
    const modules::ConfigDir config{ dir.path() / "config" };

    const auto bom{ config.resolve("bom", DEFAULTS) };
    ASSERT_TRUE(bom) << all_reasons(bom.error());
    EXPECT_FALSE(flag(*bom, "log_result"));

    const auto empty{ config.resolve("empty", DEFAULTS) };
    ASSERT_FALSE(empty);
    EXPECT_NE(all_reasons(empty.error()).find("the file is empty"), std::string::npos);
}

TEST(ModuleConfigTest, ProfileForAModuleNotInstalledIsOnlyAWarning)
{
    TempDir dir{};
    dir.write("config/profiles/extra.json", R"({ "modules": { "autofarm": { "speed": 3 } } })");
    const modules::ConfigDir config{ dir.path() / "config" };
    const auto resolved{ config.resolve("extra", DEFAULTS) };
    ASSERT_TRUE(resolved);
    ASSERT_EQ(resolved->warnings.size(), 1u);
    EXPECT_NE(resolved->warnings[0].find("'autofarm' is not installed"), std::string::npos);
}

TEST(ModuleConfigTest, ListsProfiles)
{
    TempDir dir{};
    dir.write("config/profiles/test.json", R"({ "description": "For testing" })");
    dir.write("config/profiles/default.json", "{}");
    dir.write("config/profiles/notes.txt", "not a profile");
    const modules::ConfigDir config{ dir.path() / "config" };
    EXPECT_EQ(config.profiles(), (std::vector<std::string>{ "default", "test" }));
    EXPECT_EQ(config.profile_description("test"), "For testing");
    EXPECT_FALSE(config.profile_description("default"));
}

TEST(ModuleManifestTest, DiscoversModules)
{
    TempDir dir{};
    dir.write("modules/reachability/module.json", R"({ "id": "reachability", "version": "1.0.0", "api_version": 1, "main": "main.lua",
        "settings": { "show_marker": true, "radius": 5, "label": "x" } })");
    dir.write("modules/reachability/main.lua", "-- module");
    dir.write("modules/zz_copy/module.json", R"({ "id": "reachability", "version": "2.0.0", "api_version": 1, "main": "main.lua" })");
    dir.write("modules/zz_copy/main.lua", "");
    dir.write("modules/future/module.json", R"({ "id": "future", "version": "1.0.0", "api_version": 5, "main": "main.lua" })");
    dir.write("modules/future/main.lua", "");
    dir.write("modules/v2/module.json", R"({ "id": "v2", "version": "1.0.0", "api_version": 2, "main": "main.lua" })");
    dir.write("modules/v2/main.lua", "");
    dir.write("modules/nomain/module.json", R"({ "id": "nomain", "version": "1.0.0", "api_version": 1, "main": "missing.lua" })");
    dir.write("modules/escape/module.json", R"({ "id": "escape", "version": "1.0.0", "api_version": 1, "main": "../evil.lua" })");
    dir.write("modules/badjson/module.json", R"({ "id": "badjson", )");
    dir.write("modules/badid/module.json", R"({ "id": "Bad Id", "version": "1", "api_version": 1, "main": "main.lua" })");
    dir.write("modules/not_a_module/readme.txt", "no module.json here");

    const auto found{ modules::discover(dir.path() / "modules") };
    ASSERT_EQ(found.modules.size(), 2u);
    EXPECT_EQ(found.modules[0].id, "reachability");
    EXPECT_EQ(found.modules[0].version, "1.0.0");
    EXPECT_EQ(std::get<double>(found.modules[0].settings.at("radius")), 5.0);
    EXPECT_EQ(found.modules[1].id, "v2");
    EXPECT_EQ(found.modules[1].api_version, 2);

    std::string problems;
    for (const auto& problem : found.problems) {
        problems += problem + "\n";
    }
    EXPECT_EQ(found.problems.size(), 6u) << problems;
    EXPECT_NE(problems.find("modules/zz_copy: id 'reachability' is already used by modules/reachability"), std::string::npos) << problems;
    EXPECT_NE(problems.find("modules/future: needs module API version 5, this proxy has versions 1 to 4"), std::string::npos) << problems;
    EXPECT_NE(problems.find("modules/nomain: missing.lua is missing"), std::string::npos) << problems;
    EXPECT_NE(problems.find("modules/escape: module.json: main '../evil.lua' must be a .lua file inside"), std::string::npos) << problems;
    EXPECT_NE(problems.find("modules/badjson: module.json: "), std::string::npos) << problems;
    EXPECT_NE(problems.find("modules/badid: module.json: id 'Bad Id'"), std::string::npos) << problems;
}

TEST(ModuleManifestTest, NoModulesFolder)
{
    TempDir dir{};
    const auto found{ modules::discover(dir.path() / "modules") };
    EXPECT_TRUE(found.modules.empty());
    EXPECT_TRUE(found.problems.empty());
}
