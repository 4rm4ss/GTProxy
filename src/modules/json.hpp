#pragma once
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

#include <fmt/format.h>
#include <glaze/glaze.hpp>

// Reading the JSON files of modules/ and config/: strict (unknown keys are
// errors, so a typo is reported instead of silently ignored) and with
// readable error messages.
namespace modules::json {
// Text of a file, without the UTF-8 BOM Notepad may write. Nothing if it
// cannot be read.
[[nodiscard]] inline std::optional<std::string> read_text(const std::filesystem::path& file)
{
    std::ifstream in{ file, std::ios::binary };
    if (!in) {
        return std::nullopt;
    }

    std::string text{ std::istreambuf_iterator<char>{ in }, {} };
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.erase(0, 3);
    }
    return text;
}

// "line 3, column 18: expected_true_or_false (near `"show_steps": yes`)"
[[nodiscard]] inline std::string describe(const glz::error_ctx& error, const std::string& text)
{
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) {
        return "the file is empty (it must hold a JSON object: { ... })";
    }

    const std::string full{ glz::format_error(error, text) };
    const auto first_line{ full.substr(0, full.find('\n')) };

    // glaze: "<line>:<column>: <code>" followed by the line and a caret.
    std::string reason{ first_line };
    if (const auto colon{ first_line.find(':') }, second{ first_line.find(':', first_line.find(':') + 1) };
        colon != std::string::npos && second != std::string::npos) {
        reason = fmt::format(
            "line {}, column {}: {}",
            first_line.substr(0, colon),
            first_line.substr(colon + 1, second - colon - 1),
            first_line.substr(second + 2)
        );
    }

    if (const auto context_start{ full.find('\n') }; context_start != std::string::npos) {
        std::string context{ full.substr(context_start + 1) };
        context = context.substr(0, context.find('\n'));
        const auto begin{ context.find_first_not_of(' ') };
        if (begin != std::string::npos) {
            reason += fmt::format(" (near `{}`)", context.substr(begin, 60));
        }
    }

    const std::string code{ glz::format_error(error.ec) };
    if (code == "no_matching_variant_type") {
        reason += " - a setting must be true/false, a number or text";
    }
    else if (code == "unknown_key") {
        reason += " - this key is not known (typo?)";
    }
    else if (code == "expected_brace") {
        reason += " - the file must hold a JSON object: { ... }";
    }

    return reason;
}

template <typename T>
[[nodiscard]] std::expected<T, std::string> parse(const std::string& text)
{
    T value{};
    if (const auto error{ glz::read<glz::opts{ .error_on_unknown_keys = true }>(value, text) }) {
        return std::unexpected{ describe(error, text) };
    }
    return value;
}
}
