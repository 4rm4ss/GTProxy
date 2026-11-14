#pragma once
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include <spdlog/sinks/base_sink.h>

namespace core::session_log {
// Hides login secrets (tokens, passwords, device identifiers) so a session log
// can be shared safely. Works on whole, possibly multi-line, log messages.
[[nodiscard]] std::string redact(std::string_view text);

// Writes one log file per proxy run: logs/session-YYYY-MM-DD_HH-MM-SS.txt.
// Every line is redacted. Once the file passes max_bytes only warnings and
// errors are still written, so a long session cannot fill the disk.
class Sink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    Sink(std::filesystem::path path, std::size_t max_bytes);

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override;
    void flush_() override;

private:
    std::filesystem::path path_;
    std::ofstream file_;
    std::size_t max_bytes_;
    std::size_t written_{ 0 };
    bool limit_reached_{ false };
};

// Creates logs/ (if needed), removes the oldest session logs so that at most
// `keep` remain including the new one, and returns a sink for a new file.
[[nodiscard]] std::shared_ptr<Sink> create(
    const std::filesystem::path& directory,
    std::size_t keep = 20,
    std::size_t max_bytes = 50u * 1024u * 1024u
);
}
