#include "session_log.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <ctime>
#include <system_error>
#include <vector>

#include <fmt/format.h>

namespace core::session_log {
namespace {
// Keys whose values must never end up in a shared log.
constexpr std::array SECRET_KEYS{
    "ltoken", "token", "tankidpass", "uuidtoken", "klv", "hash", "hash2", "fhash",
    "mac", "rid", "wk", "meta", "gid", "vid", "aid", "sid", "password",
};

std::string lower(std::string_view text)
{
    std::string out{ text };
    std::ranges::transform(out, out.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool is_secret_key(std::string_view key)
{
    while (!key.empty() && (key.front() == ' ' || key.front() == '\t')) {
        key.remove_prefix(1);
    }
    while (!key.empty() && key.back() == ' ') {
        key.remove_suffix(1);
    }
    const auto k{ lower(key) };
    return std::ranges::any_of(SECRET_KEYS, [&k](const char* secret) { return k == secret; });
}

// "key: value" (packet dumps) and "key|value" (raw game text) lines.
std::string redact_line(const std::string_view line)
{
    const auto separator{ line.find_first_of(":|") };
    if (separator == std::string_view::npos || separator == 0) {
        return std::string{ line };
    }

    if (!is_secret_key(line.substr(0, separator))) {
        return std::string{ line };
    }

    const auto value_start{ line[separator] == ':' && separator + 1 < line.size() && line[separator + 1] == ' '
        ? separator + 2
        : separator + 1 };
    if (value_start >= line.size()) {
        return std::string{ line };
    }

    return std::string{ line.substr(0, value_start) } + "***";
}

// OnSendToServer dump: "[2] [SIGNED]: <token>" and "[4] [STRING]: ip|door|<uuid token>".
void redact_send_to_server(std::vector<std::string>& lines)
{
    for (auto& line : lines) {
        if (line.starts_with("[2] ")) {
            if (const auto colon{ line.find(": ") }; colon != std::string::npos) {
                line = line.substr(0, colon + 2) + "***";
            }
        } else if (line.starts_with("[4] ")) {
            if (const auto last{ line.rfind('|') }; last != std::string::npos && line.find('|') != last) {
                line = line.substr(0, last + 1) + "***";
            }
        }
    }
}

std::string timestamp_for_file_name()
{
    const auto now{ std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()) };
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    return fmt::format(
        "{:04}-{:02}-{:02}_{:02}-{:02}-{:02}",
        local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec
    );
}
}

std::string redact(const std::string_view text)
{
    std::vector<std::string> lines;
    std::size_t start{ 0 };
    while (true) {
        const auto end{ text.find('\n', start) };
        lines.emplace_back(redact_line(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start)));
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }

    if (text.find("[0] [STRING]: OnSendToServer") != std::string_view::npos) {
        redact_send_to_server(lines);
    }

    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) {
            out += '\n';
        }
        out += lines[i];
    }
    return out;
}

Sink::Sink(std::filesystem::path path, const std::size_t max_bytes)
    : path_{ std::move(path) }
    , file_{ path_, std::ios::binary | std::ios::app }
    , max_bytes_{ max_bytes }
{ }

void Sink::sink_it_(const spdlog::details::log_msg& msg)
{
    if (!file_ || (limit_reached_ && msg.level < spdlog::level::warn)) {
        return;
    }

    spdlog::memory_buf_t formatted;
    formatter_->format(msg, formatted);
    const auto text{ redact({ formatted.data(), formatted.size() }) };
    file_.write(text.data(), static_cast<std::streamsize>(text.size()));
    written_ += text.size();

    if (!limit_reached_ && written_ > max_bytes_) {
        limit_reached_ = true;
        constexpr std::string_view notice{
            "--- session log size limit reached: only warnings and errors are written from now on ---\n"
        };
        file_.write(notice.data(), static_cast<std::streamsize>(notice.size()));
        file_.flush();
    }
}

void Sink::flush_()
{
    file_.flush();
}

std::shared_ptr<Sink> create(const std::filesystem::path& directory, const std::size_t keep, const std::size_t max_bytes)
{
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);

    // Prune old session logs (names sort chronologically).
    std::vector<std::filesystem::path> existing;
    for (const auto& entry : std::filesystem::directory_iterator{ directory, ec }) {
        const auto name{ entry.path().filename().string() };
        if (entry.is_regular_file(ec) && name.starts_with("session-") && name.ends_with(".txt")) {
            existing.push_back(entry.path());
        }
    }
    std::ranges::sort(existing);
    while (keep > 0 && existing.size() >= keep) {
        std::filesystem::remove(existing.front(), ec);
        existing.erase(existing.begin());
    }

    const auto stamp{ timestamp_for_file_name() };
    auto path{ directory / fmt::format("session-{}.txt", stamp) };
    for (int i = 2; std::filesystem::exists(path, ec); ++i) {
        path = directory / fmt::format("session-{}-{}.txt", stamp, i);
    }

    return std::make_shared<Sink>(path, max_bytes);
}
}
