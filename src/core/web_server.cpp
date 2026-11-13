#include "web_server.hpp"

#include <fmt/format.h>
#include <magic_enum/magic_enum.hpp>
#include <spdlog/spdlog.h>

#include "../utils/text_parse.hpp"
// ReSharper disable once CppUnusedIncludeDirective
#include "../utils/formatter/text_parse_formatter.hpp"

namespace core {
WebServer::WebServer(
    Config& config,
    event::Dispatcher& dispatcher,
    network::Client& client,
    network::Server& server
)
    : config_{ config }
    , dispatcher_{ dispatcher }
    , client_{ client }
    , server_{ server }
    , dns_resolver_{ network::create_dns_provider(config_.get_client_config().dns_server) }
    , certificates_{ certificate::ensure("resources") }
    , https_server_{ certificates_.server_cert.string().c_str(), certificates_.server_key.string().c_str() }
    , pending_port_{ 65535 }
{
    setup_server();

    if (!https_server_.is_valid()) {
        spdlog::error("HTTPS server could not load {} / {}", certificates_.server_cert.string(), certificates_.server_key.string());
        return;
    }

    if (!https_server_.bind_to_port("0.0.0.0", 443)) {
        spdlog::error(
            "Failed to bind to port 443: another program is using it (e.g. Skype, IIS, VMware, "
            "another proxy) or binding needs administrator rights on this system"
        );
        return;
    }

    dispatcher_.append_listener(event::Type::ClientConnect, [this](const event::Event& e) { on_client_connect(e); });
    dispatcher_.append_listener(event::Type::ClientDisconnect, [this](const event::Event& e) { on_client_disconnect(e); });

    spdlog::info("HTTPS server listening on port 443");
    server_thread_ = std::thread{ &WebServer::listen_internal, this };
}

WebServer::~WebServer()
{
    https_server_.stop();
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
}

void WebServer::setup_server()
{
    https_server_.set_logger([](const httplib::Request& req, const httplib::Response& res) {
        spdlog::info("{} {} {}", req.method, req.path, res.status);
    });

    https_server_.set_error_handler([](const httplib::Request&, httplib::Response& res) {
        res.set_content(
            fmt::format("{} ({})", httplib::status_message(res.status), res.status),
            "text/plain"
        );
    });

    https_server_.set_exception_handler([](const httplib::Request&, httplib::Response& res, const std::exception_ptr& ep) {
        res.status = 500;
        try {
            std::rethrow_exception(ep);
        }
        catch (const std::exception& e) {
            res.set_content(fmt::format("{}", e.what()), "text/plain");
        }
        catch (...) { // This should never happen, but just in case
            res.set_content("Unknown Exception", "text/plain");
        }
    });
}

void WebServer::listen_internal()
{
    https_server_.Post("/growtopia/server_data.php", [this](
        const httplib::Request& req,
        httplib::Response& res
    ) -> bool {
        if (!req.headers.empty()) {
            spdlog::info("Headers:");
            for (const auto& [key, value] : req.headers) {
                spdlog::info("\t{}: {}", key, value);
            }
        }

        if (!req.params.empty()) {
            spdlog::info("Params:");
            spdlog::info("\t{}", httplib::detail::params_to_query_str(req.params));
        }

        if (!req.body.empty()) {
            spdlog::info("Body:");
            spdlog::info("\t{}", req.body);
        }

        const auto upstream_body{ fetch_server_data(req) };
        if (!upstream_body) {
            res.status = 502;
            res.set_content("Failed to get server_data.php from Growtopia", "text/plain");
            return true;
        }

        utils::TextParse text_parse{ *upstream_body };
        if (text_parse.empty()) {
            spdlog::error("Failed to parse server_data.php response");
            res.status = 500;
            return true;
        }

        spdlog::info("Original server_data.php response:\n{}", text_parse);

        const std::string server_ip{ text_parse.get("server", 0) };
        const std::string port{ text_parse.get("port", 0) };
        uint16_t server_port{};
        if (
            auto [ptr, ec] = std::from_chars(port.data(), port.data() + port.size(), server_port);
            ec != std::errc{} || server_ip.empty()
        ) {
            spdlog::error("Failed to parse server/port from server_data.php response");
            res.status = 500;
            return true;
        }

        {
            // Written on the HTTPS thread, read on the main thread.
            std::lock_guard lock{ pending_mutex_ };
            pending_address_ = server_ip;
            pending_port_ = server_port;
        }

        text_parse.set("server", { "127.0.0.1" });
        text_parse.set("port", { std::to_string(config_.get_server_config().port) });
        text_parse.set("type2", { "1" });

        spdlog::debug("Modified server_data.php response:\n{}", text_parse);

        // TextParse drops lines without a key|value pair, such as the
        // "RTENDMARKERBS1001" end marker; keep it so the client sees the
        // response in the same shape the real server sends.
        std::string body{ text_parse.get_raw() };
        constexpr std::string_view end_marker{ "RTENDMARKERBS1001" };
        if (
            upstream_body->find(end_marker) != std::string::npos
            && body.find(end_marker) == std::string::npos
        ) {
            if (!body.empty() && body.back() != '\n') {
                body += '\n';
            }
            body += end_marker;
        }

        res.set_content(body, "text/html");
        return true;
    });

    https_server_.listen_after_bind();
}

std::optional<std::string> WebServer::fetch_server_data(const httplib::Request& req)
{
    // Try the configured host first; for the official hosts also try the other
    // one, as either can be unreachable or slow at times.
    const std::string& configured{ config_.get_server_config().address };
    std::vector<std::string> hosts{ configured };
    if (configured == "www.growtopia1.com") {
        hosts.emplace_back("www.growtopia2.com");
    } else if (configured == "www.growtopia2.com") {
        hosts.emplace_back("www.growtopia1.com");
    }

    const std::string content_type{
        req.has_header("Content-Type")
            ? req.get_header_value("Content-Type")
            : std::string{ "application/x-www-form-urlencoded" }
    };

    for (const auto& host : hosts) {
        const std::string ip{ dns_resolver_.resolve_ip(host) };
        if (ip.empty()) {
            spdlog::warn("server_data.php: cannot resolve {}", host);
            continue;
        }

        // Connect to the resolved IP but keep the real host name for TLS SNI and
        // the Host header, like the game does (system DNS points it at us).
        httplib::Client cli{ fmt::format("https://{}", host) };
        if (ip != host) {
            cli.set_hostname_addr_map({ { host, ip } });
        }
        cli.enable_server_certificate_verification(false);
        cli.set_connection_timeout(10);
        cli.set_read_timeout(15);

        const httplib::Headers headers{
            { "User-Agent", req.get_header_value("User-Agent") },
            { "Accept", "*/*" }
        };

        // Forward the client's form body byte for byte.
        const httplib::Result response{ cli.Post("/growtopia/server_data.php", headers, req.body, content_type) };
        if (!response) {
            spdlog::warn("server_data.php via {} ({}) failed: {}", host, ip, httplib::to_string(response.error()));
            continue;
        }

        if (response->status != 200 || response->body.find("server|") == std::string::npos) {
            spdlog::warn(
                "server_data.php via {} returned HTTP {}: {}",
                host,
                response->status,
                response->body.substr(0, 200)
            );
            continue;
        }

        spdlog::info("Got server_data.php from {} ({})", host, ip);
        return response->body;
    }

    return std::nullopt;
}

void WebServer::on_client_connect(const event::Event& e)
{
    std::string address;
    uint16_t port{};
    {
        std::lock_guard lock{ pending_mutex_ };
        if (pending_port_ == 65535) {
            return;
        }

        address = std::move(pending_address_);
        port = pending_port_;
        pending_address_.clear();
        pending_port_ = 65535;
    }

    spdlog::info("Connecting to Growtopia server at {}:{}", address, port);
    client_.connect(address, port);
}

void WebServer::on_client_disconnect(const event::Event& e)
{
    std::lock_guard lock{ pending_mutex_ };
    pending_address_.clear();
    pending_port_ = 65535;
}
}
