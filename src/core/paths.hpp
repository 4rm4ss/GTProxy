#pragma once
#include <filesystem>

namespace core::paths {
// Folder of the running executable (Proxy.exe), whatever the working
// directory is: modules/ and config/ live next to it. Falls back to the
// working directory if the system cannot tell.
[[nodiscard]] std::filesystem::path executable_dir();
}
