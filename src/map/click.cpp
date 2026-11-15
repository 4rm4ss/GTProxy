#include "click.hpp"

#include <charconv>
#include <cmath>
#include <system_error>

namespace map {
namespace {
std::optional<std::string_view> param(const std::multimap<std::string, std::string>& params, const std::string& key)
{
    const auto it{ params.find(key) };
    if (it == params.end()) {
        return std::nullopt;
    }
    return std::string_view{ it->second };
}

std::optional<float> number(const std::multimap<std::string, std::string>& params, const std::string& key)
{
    const auto text{ param(params, key) };
    if (!text || text->empty()) {
        return std::nullopt;
    }

    float value{};
    const auto* end{ text->data() + text->size() };
    const auto [ptr, ec]{ std::from_chars(text->data(), end, value) };
    if (ec != std::errc{} || ptr != end || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}
}

std::string_view describe(const ClickError error)
{
    switch (error) {
    case ClickError::NoWorld: return "you are not in a world";
    case ClickError::BadView: return "the map window sent an invalid view";
    case ClickError::OutsideWorld: return "that is outside the world";
    case ClickError::NoAction: return "this button does nothing";
    }
    return "unknown error";
}

std::optional<Click> parse_click(const std::multimap<std::string, std::string>& params)
{
    const auto x{ number(params, "x") };
    const auto y{ number(params, "y") };
    const auto button{ number(params, "button") };
    const auto ox{ number(params, "ox") };
    const auto oy{ number(params, "oy") };
    const auto zoom{ number(params, "zoom") };
    const auto w{ number(params, "w") };
    const auto h{ number(params, "h") };
    if (!x || !y || !button || !ox || !oy || !zoom || !w || !h) {
        return std::nullopt;
    }

    Click click{};
    click.screen = { *x, *y };
    click.view.origin = { *ox, *oy };
    click.view.zoom = *zoom;
    click.view.size = { *w, *h };

    if (*button == 0.0f) {
        click.button = MouseButton::Left;
    }
    else if (*button == 1.0f) {
        click.button = MouseButton::Middle;
    }
    else if (*button == 2.0f) {
        click.button = MouseButton::Right;
    }
    else {
        return std::nullopt;
    }

    return click;
}

std::optional<TilePos> parse_tile(const std::string_view x, const std::string_view y)
{
    const auto integer = [](const std::string_view text) -> std::optional<int> {
        int value{};
        const auto* end{ text.data() + text.size() };
        const auto [ptr, ec]{ std::from_chars(text.data(), end, value) };
        if (text.empty() || ec != std::errc{} || ptr != end) {
            return std::nullopt;
        }
        return value;
    };

    const auto tx{ integer(x) };
    const auto ty{ integer(y) };
    if (!tx || !ty) {
        return std::nullopt;
    }
    return TilePos{ *tx, *ty };
}

std::optional<ClickAction> action_for(const MouseButton button)
{
    switch (button) {
    case MouseButton::Left: return ClickAction::Walk;
    case MouseButton::Right: return ClickAction::Inspect;
    case MouseButton::Middle: return std::nullopt;
    }
    return std::nullopt;
}

std::expected<ClickTarget, ClickError> resolve_click(const Click& click, const glm::ivec2 world_size)
{
    if (world_size.x <= 0 || world_size.y <= 0) {
        return std::unexpected{ ClickError::NoWorld };
    }

    if (!click.view.valid()) {
        return std::unexpected{ ClickError::BadView };
    }

    const auto action{ action_for(click.button) };
    if (!action) {
        return std::unexpected{ ClickError::NoAction };
    }

    const auto tile{ screen_to_tile(click.view, click.screen, world_size) };
    if (!tile) {
        return std::unexpected{ ClickError::OutsideWorld };
    }

    return ClickTarget{ *tile, *action };
}
}
