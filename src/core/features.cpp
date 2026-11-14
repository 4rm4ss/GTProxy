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

// Calls visit(line_begin, line_end) for every line of text.
template <typename Visit>
void for_each_line(const std::string_view text, Visit&& visit)
{
    std::size_t begin{ 0 };
    while (begin <= text.size()) {
        const std::size_t end{ std::min(text.find('\n', begin), text.size()) };
        visit(begin, end);
        if (end == text.size()) {
            break;
        }
        begin = end + 1;
    }
}

bool line_has_key(const std::string_view line, const std::string_view key)
{
    return line.size() > key.size() && line.starts_with(key) && line[key.size()] == '|';
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

std::optional<std::string> normalize_door_id(const std::string_view door)
{
    if (door.empty() || door.size() > 24) {
        return std::nullopt;
    }

    std::string out;
    out.reserve(door.size());
    for (const char c : door) {
        if (!is_alnum(c) && c != '_') {
            return std::nullopt;
        }
        out += upper(c);
    }
    return out;
}

std::optional<std::string> replace_field(const std::string_view text, const std::string_view key, const std::string_view value)
{
    std::optional<std::string> result;
    for_each_line(text, [&](const std::size_t begin, const std::size_t end) {
        if (result || !line_has_key(text.substr(begin, end - begin), key)) {
            return;
        }

        std::string out{ text.substr(0, begin + key.size() + 1) };
        out += value;
        out += text.substr(end);
        result = std::move(out);
    });
    return result;
}

std::optional<std::string> field_value(const std::string_view text, const std::string_view key)
{
    std::optional<std::string> result;
    for_each_line(text, [&](const std::size_t begin, const std::size_t end) {
        const auto line{ text.substr(begin, end - begin) };
        if (!result && line_has_key(line, key)) {
            result = std::string{ line.substr(key.size() + 1) };
        }
    });
    return result;
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

std::string colored_name(const std::string_view name)
{
    const std::string clean{ sanitize(name) };
    if (clean.find('`') != std::string::npos) {
        return clean;
    }
    return "`w" + clean + "``";
}
}
