#include "bridge.hpp"

#include <utility>

namespace map {
std::optional<std::future<std::string>> Bridge::submit(const Request::Kind kind, const Click& click, std::map<std::string, std::string> params)
{
    std::lock_guard lock{ mutex_ };
    if (requests_.size() >= MAX_PENDING) {
        return std::nullopt;
    }

    Request& request{ requests_.emplace_back() };
    request.kind = kind;
    request.click = click;
    request.params = std::move(params);
    return request.reply.get_future();
}

std::shared_ptr<const std::string> Bridge::state() const
{
    std::lock_guard lock{ mutex_ };
    return state_;
}

std::shared_ptr<const std::string> Bridge::tiles() const
{
    std::lock_guard lock{ mutex_ };
    return tiles_;
}

std::shared_ptr<const std::string> Bridge::modules() const
{
    std::lock_guard lock{ mutex_ };
    return modules_;
}

void Bridge::publish_modules(std::string json)
{
    auto shared{ std::make_shared<const std::string>(std::move(json)) };
    std::lock_guard lock{ mutex_ };
    modules_ = std::move(shared);
}

void Bridge::mark_viewed()
{
    std::lock_guard lock{ mutex_ };
    last_view_ = std::chrono::steady_clock::now();
    viewed_ = true;
}

std::vector<Bridge::Request> Bridge::take_requests()
{
    std::lock_guard lock{ mutex_ };
    std::vector<Request> taken;
    taken.reserve(requests_.size());
    for (auto& request : requests_) {
        taken.push_back(std::move(request));
    }
    requests_.clear();
    return taken;
}

void Bridge::publish_state(std::string json)
{
    auto shared{ std::make_shared<const std::string>(std::move(json)) };
    std::lock_guard lock{ mutex_ };
    state_ = std::move(shared);
}

void Bridge::publish_tiles(std::string json)
{
    auto shared{ std::make_shared<const std::string>(std::move(json)) };
    std::lock_guard lock{ mutex_ };
    tiles_ = std::move(shared);
}

bool Bridge::viewed_within(const std::chrono::steady_clock::duration window) const
{
    std::lock_guard lock{ mutex_ };
    return viewed_ && std::chrono::steady_clock::now() - last_view_ <= window;
}
}
