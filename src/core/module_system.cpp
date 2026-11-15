#include "module_system.hpp"

#include <system_error>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "../map/reachability.hpp"
#include "../world/world.hpp"

namespace core {
ModuleSystem::ModuleSystem(
    const std::filesystem::path& app_dir,
    handlers::MapHandler& map,
    handlers::FeatureHandler& features,
    command::CommandRegistry& registry
)
    : modules_dir_{ app_dir / "modules" }
    , config_{ app_dir / "config" }
    , map_{ map }
    , features_{ features }
    , registry_{ registry }
    , host_{ *this }
{
    // Modules cannot take the proxy's own commands.
    host_.set_reserved_names([this](const std::string_view name) {
        return name == "cfg" || name == "help"
            || (registry_.get(name) != nullptr && !chat_commands_.contains(std::string{ name }));
    });
    host_.set_on_change([this] { sync_commands(); });

    spdlog::info("Modules: {} | settings: {}", modules_dir_.string(), config_.dir().string());

    auto found{ modules::discover(modules_dir_) };
    for (const auto& problem : found.problems) {
        spdlog::error("Module skipped: {}", problem);
    }
    if (std::error_code ec; !std::filesystem::is_directory(modules_dir_, ec)) {
        spdlog::info("No modules/ folder next to the proxy: running without modules");
    }
    host_.set_available(std::move(found.modules));

    const auto modules_file{ config_.read_modules_file() };
    if (modules_file) {
        profile_ = modules_file->profile;
    }
    else {
        spdlog::error("Config error: {}: {}", modules_file.error().file, modules_file.error().reason);
    }

    if (!apply_profile(profile_, "start")) {
        // Nothing that worked before to fall back on: the modules' own defaults.
        modules::Resolved defaults{};
        defaults.profile = profile_;
        for (const auto& manifest : host_.available()) {
            defaults.settings[manifest.id] = manifest.settings;
            bool on{ true };
            if (modules_file) {
                if (const auto it{ modules_file->modules.find(manifest.id) }; it != modules_file->modules.end()) {
                    on = it->second.enabled;
                }
            }
            defaults.enabled[manifest.id] = on;
        }
        enabled_ = defaults.enabled;
        host_.apply(defaults);
        spdlog::warn("Modules use their default settings until the config is fixed ('cfg reload' in this console)");
        log_settings();
    }

    map_.set_right_click_hook([this](const map::TilePos tile) {
        in_click_ = true;
        card_.reset();
        const auto ran{ host_.right_click(tile, features_.in_world()) };
        in_click_ = false;
        return handlers::MapHandler::RightClickAnswer{ ran > 0, std::exchange(card_, std::nullopt) };
    });
}

ModuleSystem::~ModuleSystem()
{
    map_.set_right_click_hook({});
    host_.set_on_change({});
    for (const auto& name : chat_commands_) {
        registry_.remove(name);
    }
}

bool ModuleSystem::run_console(const std::vector<std::string>& words)
{
    if (words.empty()) {
        return false;
    }
    if (words[0] == "cfg") {
        cfg(words);
        return true;
    }
    return host_.run_command(words[0], { words.begin() + 1, words.end() }, modules::Source::Console);
}

std::vector<std::string> ModuleSystem::console_help() const
{
    std::vector<std::string> lines{
        "cfg list - the profiles in config/profiles",
        "cfg exec <profile> - apply config/profiles/<profile>.json",
        "cfg reload - read config/ again for the active profile",
    };
    for (const auto& command : host_.commands()) {
        lines.push_back(fmt::format("{} - {} (module {})", command.name, command.description, command.module));
    }
    return lines;
}

void ModuleSystem::cfg(const std::vector<std::string>& words)
{
    if (words.size() == 2 && words[1] == "list") {
        cfg_list();
        return;
    }
    if (words.size() == 3 && words[1] == "exec") {
        apply_profile(words[2], "cfg exec");
        return;
    }
    if (words.size() == 2 && words[1] == "reload") {
        apply_profile(profile_, "cfg reload");
        return;
    }
    spdlog::info("Usage: cfg list | cfg exec <profile> | cfg reload (active profile: {})", profile_);
}

void ModuleSystem::cfg_list() const
{
    const auto names{ config_.profiles() };
    if (names.empty()) {
        spdlog::info("No profiles in {} (active: {})", (config_.dir() / "profiles").string(), profile_);
        return;
    }

    spdlog::info("Profiles in config/profiles (* = active):");
    for (const auto& name : names) {
        const auto description{ config_.profile_description(name) };
        spdlog::info(
            "  {} {}{}",
            name == profile_ ? '*' : ' ',
            name,
            description ? fmt::format(" - {}", *description) : std::string{}
        );
    }
}

bool ModuleSystem::apply_profile(const std::string& profile, const std::string_view why)
{
    const auto resolved{ config_.resolve(profile, host_.defaults()) };
    if (!resolved) {
        for (const auto& error : resolved.error()) {
            spdlog::error("Config error: {}: {}", error.file, error.reason);
        }
        if (why != "start") {
            spdlog::warn("{} '{}' failed: the settings in force are kept (profile '{}')", why, profile, profile_);
        }
        return false;
    }

    for (const auto& warning : resolved->warnings) {
        spdlog::warn("Config: {}", warning);
    }

    enabled_ = resolved->enabled;
    host_.apply(*resolved);
    profile_ = profile;
    spdlog::info("Profile '{}' applied ({})", profile_, why);
    log_settings();
    return true;
}

void ModuleSystem::log_settings() const
{
    for (const auto& manifest : host_.available()) {
        const auto it{ enabled_.find(manifest.id) };
        const bool on{ it == enabled_.end() || it->second };
        const char* state{ host_.loaded(manifest.id) ? "on" : (on ? "failed to load" : "off") };

        std::string values;
        if (const auto settings{ host_.settings(manifest.id) }) {
            for (const auto& [key, value] : *settings) {
                values += fmt::format("{}{}={}", values.empty() ? "" : ", ", key, modules::to_string(value));
            }
        }
        spdlog::info("Module {} {}: {} | {}", manifest.id, manifest.version, state, values.empty() ? "no settings" : values);
    }
}

void ModuleSystem::sync_commands()
{
    std::set<std::string> wanted;
    for (const auto& command : host_.commands()) {
        wanted.insert(command.name);
    }

    for (auto it{ chat_commands_.begin() }; it != chat_commands_.end();) {
        if (!wanted.contains(*it)) {
            registry_.remove(*it);
            it = chat_commands_.erase(it);
        }
        else {
            ++it;
        }
    }

    for (const auto& command : host_.commands()) {
        if (chat_commands_.contains(command.name)) {
            continue;
        }
        if (registry_.get(command.name)) {
            spdlog::error("[{}] /{} is a command of the proxy; the module's one is not added", command.module, command.name);
            continue;
        }
        registry_.add(command.name, command.description, [this, name = command.name](const command::Context& ctx) {
            host_.run_command(name, ctx.args, modules::Source::Game);
            return command::Result::Success;
        });
        chat_commands_.insert(command.name);
    }
}

modules::WorldView ModuleSystem::world() const
{
    modules::WorldView view{};
    view.in_world = features_.in_world();
    if (!view.in_world) {
        return view;
    }

    const auto& world{ world::World::instance() };
    view.name = features_.current_world();
    view.width = world.get_tile_map().get_size().x;
    view.height = world.get_tile_map().get_size().y;
    if (const auto me{ world.get_local_player().lock() }) {
        const glm::vec2 position{ me->position() };
        const auto tile{ map::body_tile(position, map::Body::from_colrect(me->collision())) };
        view.me = modules::PlayerView{ position.x, position.y, tile.x, tile.y };
    }
    return view;
}

modules::ReachView ModuleSystem::reach(const map::TilePos tile)
{
    const auto report{ map_.check_reach(tile) };
    modules::ReachView view{};
    view.x = tile.x;
    view.y = tile.y;
    view.status = map::code(report.result.status);
    view.headline = map::headline(report.result.status);
    view.reason = map::reason(report.result.status);
    view.message = report.message();
    view.missing = report.missing;
    view.start = report.result.start;
    view.steps = report.result.steps;
    view.steps_word = map::steps_word(report.result.steps);
    view.tile_text = report.tile_text;
    view.summary = report.summary();
    return view;
}

bool ModuleSystem::show(const map::Card& card)
{
    if (!in_click_) {
        return false;
    }
    card_ = card;
    return true;
}

void ModuleSystem::reply(const modules::Source source, const std::string& module_id, const std::string& text)
{
    if (source == modules::Source::Game) {
        features_.message(text);
        return;
    }
    spdlog::info("[{}] {}", module_id, text);
}
}
