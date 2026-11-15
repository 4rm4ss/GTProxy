#pragma once
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace core {
// Lines typed into the proxy's own console window. They are read on a
// background thread (reading blocks) and handed to the main loop, which runs
// them; when there is no console input (stdin closed or redirected from
// nothing) the thread simply ends.
class ConsoleInput {
public:
    ConsoleInput();

    ConsoleInput(const ConsoleInput&) = delete;
    ConsoleInput& operator=(const ConsoleInput&) = delete;

    // Lines typed since the last call (without the line break).
    [[nodiscard]] std::vector<std::string> take_lines();

private:
    // Shared with the reading thread, which may outlive this object: it
    // waits for input until the process ends.
    struct Lines {
        std::mutex mutex;
        std::deque<std::string> queue;
    };

    std::shared_ptr<Lines> lines_;
};
}
