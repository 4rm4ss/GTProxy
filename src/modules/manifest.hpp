#pragma once
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

#include "settings.hpp"

// modules/<folder>/module.json: what a module is and what it needs.
//
//   {
//     "id": "reachability",          unique, a-z 0-9 _ -
//     "version": "1.0.0",            the module's own version
//     "api_version": 1,              module API it is written for
//     "main": "main.lua",            Lua file to run, inside the folder
//     "name": "...", "description": "...", "author": "...",   optional
//     "settings": { "show_marker": true, ... }                 defaults
//   }
namespace modules {
// Versions of the module API this proxy offers (docs/modules.md). Version 2
// only adds to version 1, so modules written for 1 run unchanged. A module
// that asks for a version outside this range is not loaded.
inline constexpr int MIN_API_VERSION{ 1 };
inline constexpr int API_VERSION{ 2 };

struct Manifest {
    std::string id;
    std::string version;
    int api_version{ 0 };
    std::string main;
    std::string name;
    std::string description;
    std::string author;
    Settings settings;              // defaults
    std::filesystem::path dir;      // the module's folder

    [[nodiscard]] std::filesystem::path entry() const { return dir / main; }
};

// Reads and checks modules/<folder>/module.json.
[[nodiscard]] std::expected<Manifest, std::string> read_manifest(const std::filesystem::path& dir);

struct Discovery {
    std::vector<Manifest> modules;      // usable ones, by folder name
    std::vector<std::string> problems;  // folders skipped, and why
};

// Every folder of modules/ with a module.json. Folders are taken in name
// order; a folder whose id is already taken is skipped.
[[nodiscard]] Discovery discover(const std::filesystem::path& modules_dir);
}
