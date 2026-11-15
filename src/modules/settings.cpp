#include "settings.hpp"

#include <algorithm>
#include <system_error>
#include <utility>

#include <fmt/format.h>

#include "json.hpp"

namespace modules {
namespace {
bool name_chars(const std::string_view name, const bool upper_case_ok)
{
    if (name.empty() || name.size() > 64) {
        return false;
    }
    return std::ranges::all_of(name, [&](const char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'
            || (upper_case_ok && c >= 'A' && c <= 'Z');
    });
}

std::string known_keys(const Settings& defaults)
{
    std::string keys;
    for (const auto& [key, value] : defaults) {
        keys += keys.empty() ? key : ", " + key;
    }
    return keys.empty() ? std::string{ "none" } : keys;
}

// Lays 'overrides' over 'into'. Every key must be a setting the module has,
// with the type of its default.
void apply_overrides(
    Settings& into,
    const Settings& defaults,
    const Settings& overrides,
    const std::string& file,
    const std::string& prefix,
    std::vector<ConfigError>& errors
)
{
    for (const auto& [key, value] : overrides) {
        const auto it{ defaults.find(key) };
        if (it == defaults.end()) {
            errors.push_back({ file, fmt::format("{}{}: no such setting (known: {})", prefix, key, known_keys(defaults)) });
            continue;
        }
        if (it->second.index() != value.index()) {
            errors.push_back({ file, fmt::format(
                "{}{}: must be {}, not {} ({})",
                prefix,
                key,
                type_name(it->second),
                type_name(value),
                to_string(value)
            ) });
            continue;
        }
        into[key] = value;
    }
}

template <typename T>
std::expected<T, ConfigError> read_file(const std::filesystem::path& file, const std::string& shown)
{
    const auto text{ json::read_text(file) };
    if (!text) {
        return std::unexpected{ ConfigError{ shown, "cannot be read" } };
    }

    auto parsed{ json::parse<T>(*text) };
    if (!parsed) {
        return std::unexpected{ ConfigError{ shown, parsed.error() } };
    }
    return std::move(*parsed);
}
}

std::string_view type_name(const SettingValue& value)
{
    switch (value.index()) {
    case 0: return "true or false";
    case 1: return "a number";
    default: return "text";
    }
}

std::string to_string(const SettingValue& value)
{
    if (const auto* flag{ std::get_if<bool>(&value) }) {
        return *flag ? "true" : "false";
    }
    if (const auto* number{ std::get_if<double>(&value) }) {
        return fmt::format("{}", *number);
    }
    return fmt::format("\"{}\"", std::get<std::string>(value));
}

bool valid_profile_name(const std::string_view name)
{
    return name_chars(name, true);
}

bool valid_module_id(const std::string_view id)
{
    return name_chars(id, false);
}

ConfigDir::ConfigDir(std::filesystem::path dir)
    : dir_{ std::move(dir) }
{ }

std::string ConfigDir::shown(const std::filesystem::path& file) const
{
    std::error_code ec;
    const auto relative{ std::filesystem::relative(file, dir_, ec) };
    const auto inside{ ec || relative.empty() ? file.filename() : relative };
    return "config/" + inside.generic_string();
}

std::expected<ModulesFile, ConfigError> ConfigDir::read_modules_file() const
{
    const auto file{ dir_ / "modules.json" };
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        return ModulesFile{};
    }

    auto modules_file{ read_file<ModulesFile>(file, shown(file)) };
    if (modules_file && !valid_profile_name(modules_file->profile)) {
        return std::unexpected{ ConfigError{ shown(file), fmt::format("profile: '{}' is not a valid profile name", modules_file->profile) } };
    }
    return modules_file;
}

std::expected<Resolved, std::vector<ConfigError>> ConfigDir::resolve(
    const std::string_view profile,
    const std::map<std::string, Settings>& defaults
) const
{
    std::vector<ConfigError> errors;
    Resolved resolved{};
    resolved.profile = profile;

    const auto modules_file{ read_modules_file() };
    if (!modules_file) {
        errors.push_back(modules_file.error());
    }

    // The profile. "default" may be missing: it changes nothing.
    ProfileFile profile_file{};
    const auto profile_path{ dir_ / "profiles" / fmt::format("{}.json", profile) };
    if (!valid_profile_name(profile)) {
        errors.push_back({ fmt::format("config/profiles/{}.json", profile), "not a valid profile name (letters, digits, '_' and '-')" });
    }
    else if (std::error_code ec; std::filesystem::exists(profile_path, ec)) {
        if (auto read{ read_file<ProfileFile>(profile_path, shown(profile_path)) }) {
            profile_file = std::move(*read);
        }
        else {
            errors.push_back(read.error());
        }
    }
    else if (profile != "default") {
        errors.push_back({ shown(profile_path), "no such profile ('cfg list' shows the profiles)" });
    }

    for (const auto& [id, module_defaults] : defaults) {
        Settings settings{ module_defaults };

        const auto module_path{ dir_ / "modules" / fmt::format("{}.json", id) };
        if (std::error_code ec; std::filesystem::exists(module_path, ec)) {
            if (const auto overrides{ read_file<Settings>(module_path, shown(module_path)) }) {
                apply_overrides(settings, module_defaults, *overrides, shown(module_path), "", errors);
            }
            else {
                errors.push_back(overrides.error());
            }
        }

        if (const auto it{ profile_file.modules.find(id) }; it != profile_file.modules.end()) {
            apply_overrides(settings, module_defaults, it->second, shown(profile_path), fmt::format("modules.{}.", id), errors);
        }

        resolved.settings[id] = std::move(settings);

        bool enabled{ true };
        if (modules_file) {
            if (const auto it{ modules_file->modules.find(id) }; it != modules_file->modules.end()) {
                enabled = it->second.enabled;
            }
        }
        resolved.enabled[id] = enabled;
    }

    for (const auto& [id, settings] : profile_file.modules) {
        if (!defaults.contains(id)) {
            resolved.warnings.push_back(fmt::format(
                "{}: module '{}' is not installed, its settings are ignored",
                shown(profile_path),
                id
            ));
        }
    }

    if (!errors.empty()) {
        return std::unexpected{ std::move(errors) };
    }
    return resolved;
}

std::vector<std::string> ConfigDir::profiles() const
{
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator{ dir_ / "profiles", ec }) {
        const auto& path{ entry.path() };
        if (entry.is_regular_file(ec) && path.extension() == ".json" && valid_profile_name(path.stem().string())) {
            names.push_back(path.stem().string());
        }
    }
    std::ranges::sort(names);
    return names;
}

std::optional<std::string> ConfigDir::profile_description(const std::string_view name) const
{
    if (!valid_profile_name(name)) {
        return std::nullopt;
    }
    const auto path{ dir_ / "profiles" / fmt::format("{}.json", name) };
    const auto profile{ read_file<ProfileFile>(path, shown(path)) };
    if (!profile || profile->description.empty()) {
        return std::nullopt;
    }
    return profile->description;
}
}
