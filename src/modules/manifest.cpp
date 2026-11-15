#include "manifest.hpp"

#include <algorithm>
#include <map>
#include <system_error>

#include <fmt/format.h>

#include "json.hpp"

namespace modules {
namespace {
struct ManifestJson {
    std::string id;
    std::string version;
    int api_version{ 0 };
    std::string main;
    std::string name;
    std::string description;
    std::string author;
    Settings settings;
};

std::string folder(const std::filesystem::path& dir)
{
    return "modules/" + dir.filename().generic_string();
}
}

std::expected<Manifest, std::string> read_manifest(const std::filesystem::path& dir)
{
    const auto file{ dir / "module.json" };
    const auto text{ json::read_text(file) };
    if (!text) {
        return std::unexpected{ "module.json cannot be read" };
    }

    const auto parsed{ json::parse<ManifestJson>(*text) };
    if (!parsed) {
        return std::unexpected{ fmt::format("module.json: {}", parsed.error()) };
    }

    Manifest manifest{
        parsed->id,
        parsed->version,
        parsed->api_version,
        parsed->main,
        parsed->name,
        parsed->description,
        parsed->author,
        parsed->settings,
        dir,
    };

    if (!valid_module_id(manifest.id)) {
        return std::unexpected{ fmt::format("module.json: id '{}' is missing or invalid (use a-z, 0-9, '_' and '-')", manifest.id) };
    }
    if (manifest.version.empty()) {
        return std::unexpected{ "module.json: version is missing" };
    }
    if (manifest.api_version < MIN_API_VERSION || manifest.api_version > API_VERSION) {
        return std::unexpected{ fmt::format(
            "needs module API version {}, this proxy has versions {} to {}: not loaded",
            manifest.api_version,
            MIN_API_VERSION,
            API_VERSION
        ) };
    }

    const std::filesystem::path main{ manifest.main };
    const bool inside{
        !manifest.main.empty() && main.is_relative() && !main.has_root_name()
        && std::ranges::none_of(main, [](const std::filesystem::path& part) { return part == ".."; })
    };
    if (!inside || main.extension() != ".lua") {
        return std::unexpected{ fmt::format("module.json: main '{}' must be a .lua file inside the module's folder", manifest.main) };
    }
    if (std::error_code ec; !std::filesystem::is_regular_file(manifest.entry(), ec)) {
        return std::unexpected{ fmt::format("{} is missing", manifest.main) };
    }

    return manifest;
}

Discovery discover(const std::filesystem::path& modules_dir)
{
    Discovery found{};

    std::vector<std::filesystem::path> folders;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator{ modules_dir, ec }) {
        if (entry.is_directory(ec) && std::filesystem::exists(entry.path() / "module.json", ec)) {
            folders.push_back(entry.path());
        }
    }
    std::ranges::sort(folders);

    std::map<std::string, std::filesystem::path> taken;
    for (const auto& dir : folders) {
        auto manifest{ read_manifest(dir) };
        if (!manifest) {
            found.problems.push_back(fmt::format("{}: {}", folder(dir), manifest.error()));
            continue;
        }

        if (const auto it{ taken.find(manifest->id) }; it != taken.end()) {
            found.problems.push_back(fmt::format(
                "{}: id '{}' is already used by {}: not loaded",
                folder(dir),
                manifest->id,
                folder(it->second)
            ));
            continue;
        }

        taken.emplace(manifest->id, dir);
        found.modules.push_back(std::move(*manifest));
    }

    return found;
}
}
