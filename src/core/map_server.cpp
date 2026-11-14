#include "map_server.hpp"

#include <chrono>
#include <future>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "../map/click.hpp"
#include "../map/page.hpp"

namespace core {
namespace {
constexpr auto REPLY_TIMEOUT{ std::chrono::seconds{ 3 } };
constexpr const char* JSON{ "application/json; charset=utf-8" };

void no_cache(httplib::Response& res)
{
    res.set_header("Cache-Control", "no-store");
}

// Waits for the main loop's answer to a window request.
void answer(httplib::Response& res, std::optional<std::future<std::string>> reply)
{
    if (!reply) {
        res.status = 503;
        res.set_content("Too many clicks at once", "text/plain");
        return;
    }

    if (reply->wait_for(REPLY_TIMEOUT) != std::future_status::ready) {
        res.status = 504;
        res.set_content("The proxy is busy", "text/plain");
        return;
    }

    no_cache(res);
    res.set_content(reply->get(), JSON);
}
}

MapServer::MapServer(const Config::MapConfig& config, map::Bridge& bridge)
    : bridge_{ bridge }
    , port_{ config.port }
{
    if (!config.enabled) {
        spdlog::info("Map window is turned off (config.json: map.enabled)");
        return;
    }

    if (port_ <= 0 || port_ > 65535) {
        spdlog::error("Map window: invalid port {} (config.json: map.port)", port_);
        return;
    }

    setup_routes();

    if (!server_.bind_to_port("127.0.0.1", port_)) {
        spdlog::error(
            "Map window: port {} is in use (another GTProxy still open?); change map.port in config.json",
            port_
        );
        return;
    }

    running_ = true;
    thread_ = std::thread{ [this] { server_.listen_after_bind(); } };
    spdlog::info("Map window: {} (open it with /map in the game)", *url());
}

MapServer::~MapServer()
{
    server_.stop();
    if (thread_.joinable()) {
        thread_.join();
    }
}

std::optional<std::string> MapServer::url() const
{
    if (!running_) {
        return std::nullopt;
    }
    return fmt::format("http://127.0.0.1:{}/", port_);
}

bool MapServer::allowed_host(const httplib::Request& req) const
{
    const std::string host{ req.get_header_value("Host") };
    return host == fmt::format("127.0.0.1:{}", port_) || host == fmt::format("localhost:{}", port_);
}

void MapServer::setup_routes()
{
    server_.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
        if (!allowed_host(req)) {
            res.status = 403;
            res.set_content("Open the map at 127.0.0.1", "text/plain");
            return httplib::Server::HandlerResponse::Handled;
        }

        if (req.method == "POST" && req.get_header_value("X-GTProxy-Map") != "1") {
            res.status = 403;
            res.set_content("Missing X-GTProxy-Map header", "text/plain");
            return httplib::Server::HandlerResponse::Handled;
        }

        return httplib::Server::HandlerResponse::Unhandled;
    });

    server_.Get("/", [](const httplib::Request&, httplib::Response& res) {
        no_cache(res);
        res.set_header("X-Content-Type-Options", "nosniff");
        res.set_header("Content-Security-Policy", "default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'");
        res.set_content(map::page_html(), "text/html; charset=utf-8");
    });

    server_.Get("/api/state", [this](const httplib::Request&, httplib::Response& res) {
        bridge_.mark_viewed();
        no_cache(res);
        res.set_content(*bridge_.state(), JSON);
    });

    server_.Get("/api/tiles", [this](const httplib::Request&, httplib::Response& res) {
        bridge_.mark_viewed();
        no_cache(res);
        res.set_content(*bridge_.tiles(), JSON);
    });

    server_.Post("/api/click", [this](const httplib::Request& req, httplib::Response& res) {
        const auto click{ map::parse_click(req.params) };
        if (!click) {
            res.status = 400;
            res.set_content("Expected x, y, button, ox, oy, zoom, w, h", "text/plain");
            return;
        }
        answer(res, bridge_.submit(map::Bridge::Request::Kind::Click, *click));
    });

    server_.Post("/api/stop", [this](const httplib::Request&, httplib::Response& res) {
        answer(res, bridge_.submit(map::Bridge::Request::Kind::Stop));
    });
}
}
