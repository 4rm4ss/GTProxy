#include "features.hpp"

#include <algorithm>
#include <cctype>

namespace core::features {
namespace {
bool is_alnum(const char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) != 0;
}

char upper(const char c)
{
    return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
}
}

std::optional<std::string> normalize_world_name(const std::string_view name)
{
    if (name.empty() || name.size() > 23) {
        return std::nullopt;
    }

    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        if (!is_alnum(c)) {
            return std::nullopt;
        }
        out += upper(c);
    }
    return out;
}

std::string strip_colors(const std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i{ 0 }; i < text.size(); ++i) {
        if (text[i] == '`') {
            ++i; // skip the colour character after the backtick
            continue;
        }
        out += text[i];
    }
    return out;
}

std::string sanitize(const std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c != '|' && c != '\n' && c != '\r') {
            out += c;
        }
    }
    return out;
}
}
