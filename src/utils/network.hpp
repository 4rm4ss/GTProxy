#pragma once
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <format>
#include <vector>

#include "text_parse.hpp"

namespace utils::network {
enum class HostType {
    IpAddress,
    Hostname
};

inline bool is_valid_ip_address(const std::string& address)
{
    std::vector parts{ utils::TextParse::tokenize(address, ".") };
    if (parts.size() != 4) {
        return false;
    }

    // Each part must be a plain decimal number 0-255 ("127.0.0.1", "10.0.0.5").
    return std::ranges::all_of(parts, [](const std::string_view& part) {
        if (part.empty() || part.size() > 3) {
            return false;
        }

        std::uint8_t value{};
        const auto [ptr, ec]{ std::from_chars(part.data(), part.data() + part.size(), value) };
        return ec == std::errc{} && ptr == part.data() + part.size();
    });
}

constexpr HostType classify_host(const std::string& host)
{
    return is_valid_ip_address(host)
        ? HostType::IpAddress
        : HostType::Hostname;
}

inline std::string format_ip_address(const std::uint32_t ip_address)
{
    return std::format(
        "{}.{}.{}.{}",
        ip_address & 0xFF,
        (ip_address >> 8) & 0xFF,
        (ip_address >> 16) & 0xFF,
        (ip_address >> 24) & 0xFF
    );
}
}
