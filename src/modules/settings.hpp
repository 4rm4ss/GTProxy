#pragma once
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// Settings of modules and the profiles that change them.
//
//   config/modules.json               which modules are on, start profile
//   config/modules/<id>.json          a module's settings
//   config/profiles/<name>.json       overrides for some settings
//
// The settings in force are always computed from scratch, in this order:
// the module's defaults (module.json) -> config/modules/<id>.json -> the
// active profile. Switching profiles therefore never leaves values of the
// previous profile behind.
namespace modules {
// A setting is true/false, a number or text; its type is fixed by the
// module's default value.
using SettingValue = std::variant<bool, double, std::string>;
using Settings = std::map<std::string, SettingValue>;

// "true or false", "a number", "text"
[[nodiscard]] std::string_view type_name(const SettingValue& value);
[[nodiscard]] std::string to_string(const SettingValue& value);

struct ConfigError {
    std::string file;       // as shown to you, e.g. "config/profiles/test.json"
    std::string reason;
};

// config/modules.json
struct ModuleSwitch {
    bool enabled{ true };
};

struct ModulesFile {
    std::string profile{ "default" };
    // Modules not listed are on: copying a module's folder is enough.
    std::map<std::string, ModuleSwitch> modules;
};

// config/profiles/<name>.json
struct ProfileFile {
    std::string description;
    std::map<std::string, Settings> modules;
};

// Profile names: letters, digits, '_' and '-' (no paths).
[[nodiscard]] bool valid_profile_name(std::string_view name);
// Module ids: lower-case letters, digits, '_' and '-'.
[[nodiscard]] bool valid_module_id(std::string_view id);

struct Resolved {
    std::string profile;
    std::map<std::string, bool> enabled;            // per installed module
    std::map<std::string, Settings> settings;       // per installed module
    std::vector<std::string> warnings;
};

class ConfigDir {
public:
    explicit ConfigDir(std::filesystem::path dir);

    [[nodiscard]] const std::filesystem::path& dir() const { return dir_; }

    // config/modules.json; a missing file gives the defaults.
    [[nodiscard]] std::expected<ModulesFile, ConfigError> read_modules_file() const;

    // The settings in force for 'profile', for the installed modules and
    // their defaults. Any broken file or wrong value makes it fail with
    // every problem found: the caller keeps what it had.
    [[nodiscard]] std::expected<Resolved, std::vector<ConfigError>> resolve(
        std::string_view profile,
        const std::map<std::string, Settings>& defaults
    ) const;

    // Names of config/profiles/*.json, sorted.
    [[nodiscard]] std::vector<std::string> profiles() const;
    // The "description" of a profile, if it has one and the file reads.
    [[nodiscard]] std::optional<std::string> profile_description(std::string_view name) const;

    // "config/profiles/test.json" for a file of this folder.
    [[nodiscard]] std::string shown(const std::filesystem::path& file) const;

private:
    std::filesystem::path dir_;
};
}
