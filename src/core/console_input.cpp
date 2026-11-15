#include "console_input.hpp"

#include <iostream>
#include <thread>
#include <utility>

namespace core {
namespace {
constexpr std::size_t MAX_PENDING{ 64 };
}

ConsoleInput::ConsoleInput()
    : lines_{ std::make_shared<Lines>() }
{
    std::thread{ [lines = lines_] {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            std::lock_guard lock{ lines->mutex };
            if (lines->queue.size() < MAX_PENDING) {
                lines->queue.push_back(std::move(line));
            }
        }
    } }.detach();
}

std::vector<std::string> ConsoleInput::take_lines()
{
    std::lock_guard lock{ lines_->mutex };
    std::vector<std::string> taken{ std::make_move_iterator(lines_->queue.begin()), std::make_move_iterator(lines_->queue.end()) };
    lines_->queue.clear();
    return taken;
}
}
