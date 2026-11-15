#include "paths.hpp"

#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace core::paths {
namespace {
std::filesystem::path executable_path()
{
#ifdef _WIN32
    std::vector<wchar_t> buffer(MAX_PATH);
    while (true) {
        const DWORD length{ GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size())) };
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            return std::filesystem::path{ std::wstring{ buffer.data(), length } };
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    std::uint32_t size{ 0 };
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return {};
    }
    std::error_code ec;
    const auto resolved{ std::filesystem::weakly_canonical(buffer.c_str(), ec) };
    return ec ? std::filesystem::path{ buffer.c_str() } : resolved;
#else
    std::error_code ec;
    const auto path{ std::filesystem::read_symlink("/proc/self/exe", ec) };
    return ec ? std::filesystem::path{} : path;
#endif
}
}

std::filesystem::path executable_dir()
{
    if (const auto path{ executable_path() }; !path.empty() && path.has_parent_path()) {
        return path.parent_path();
    }

    std::error_code ec;
    return std::filesystem::current_path(ec);
}
}
