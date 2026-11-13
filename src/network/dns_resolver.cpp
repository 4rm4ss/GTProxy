#include "dns_resolver.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <system_error>
#include <httplib.h>
#include <fmt/format.h>
#include <glaze/glaze.hpp>
#include <magic_enum/magic_enum.hpp>
#include <spdlog/spdlog.h>

#include "dns_response.hpp"
#include "../utils/network.hpp"

namespace network {
namespace {
// LibreSSL from Conan looks for root certificates in a relative "res/cert.pem",
// so on macOS/Linux point it at the system CA bundle explicitly. Windows uses
// the system certificate store through cpp-httplib.
std::optional<std::string> system_ca_bundle()
{
#ifdef _WIN32
    return std::nullopt;
#else
    if (const char* env{ std::getenv("SSL_CERT_FILE") }; env && *env) {
        return std::string{ env };
    }

    static constexpr std::array candidates{
        "/etc/ssl/cert.pem",                                    // macOS, Alpine, Arch
        "/etc/ssl/certs/ca-certificates.crt",                   // Debian, Ubuntu
        "/etc/pki/tls/certs/ca-bundle.crt",                     // Fedora, RHEL
        "/etc/ssl/ca-bundle.pem",                               // openSUSE
        "/opt/homebrew/etc/ca-certificates/cert.pem",           // Homebrew (Apple silicon)
        "/usr/local/etc/ca-certificates/cert.pem",              // Homebrew (Intel)
    };

    for (const auto* candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec)) {
            return std::string{ candidate };
        }
    }

    return std::nullopt;
#endif
}
}

std::unique_ptr<DnsProvider> create_dns_provider(const std::string& name)
{
    if (name == "cloudflare") {
        return std::make_unique<CloudflareDnsProvider>();
    }

    if (name == "google") {
        return std::make_unique<GoogleDnsProvider>();
    }

    spdlog::warn("Unknown DNS provider: {}, defaulting to Google DNS", name);
    return std::make_unique<GoogleDnsProvider>();
}

DnsResolver::DnsResolver(std::unique_ptr<DnsProvider> provider)
    : provider_{ std::move(provider) }
{

}

DnsResult DnsResolver::resolve_domain(const std::string& domain) const
{
    return resolve_domain(*provider_, domain);
}

DnsResult DnsResolver::resolve_domain(const DnsProvider& provider, const std::string& domain)
{
    httplib::Headers headers{
        {"Accept", "application/dns-json"}
    };

    httplib::Client cli{ fmt::format("https://{}", provider.get_host()) };
    if (static const auto ca_bundle{ system_ca_bundle() }; ca_bundle) {
        cli.set_ca_cert_path(*ca_bundle);
    }

    cli.set_connection_timeout(5);
    cli.set_read_timeout(10);
    cli.set_write_timeout(5);

    httplib::Result res{ cli.Get(
        fmt::format("{}?name={}&type=A", provider.get_path(), domain),
        headers
    ) };

    if (!res) {
        spdlog::error(
            "DNS request failed: httplib::Error::{}",
            magic_enum::enum_name(res.error())
        );
        return { DnsStatus::ServerFail, {} };
    }

    if (res.error() != httplib::Error::Success || res->status != 200) {
        spdlog::error(
            "Failed to get DNS response: {}",
            res.error() == httplib::Error::Success
                ? fmt::format("HTTP status: {}", res->status)
                : fmt::format("HTTP error: {}", httplib::to_string(res.error()))
        );
        return { DnsStatus::ServerFail, {} };
    }

    if (res->body.empty()) {
        return { DnsStatus::ServerFail, {} };
    }

    return parse_dns_json(res->body);
}

std::string DnsResolver::resolve_ip(const std::string& host) const
{
    if (utils::network::classify_host(host) != utils::network::HostType::Hostname) {
        return host;
    }

    const auto primary{ resolve_domain(*provider_, host) };
    if (primary.status == DnsStatus::NoError) {
        spdlog::info("Resolved {} to {}", host, primary.ip);
        return primary.ip;
    }

    spdlog::error(
        "DNS resolution failed for {} via {}: {}",
        host,
        provider_->get_host(),
        magic_enum::enum_name(primary.status)
    );

    // The configured DNS-over-HTTPS provider may be unreachable from some
    // networks; try the other one before giving up.
    for (const auto* name : { "google", "cloudflare" }) {
        const auto fallback{ create_dns_provider(name) };
        if (fallback->get_host() == provider_->get_host()) {
            continue;
        }

        const auto result{ resolve_domain(*fallback, host) };
        if (result.status == DnsStatus::NoError) {
            spdlog::info("Resolved {} to {} via fallback {}", host, result.ip, fallback->get_host());
            return result.ip;
        }

        spdlog::error(
            "DNS resolution failed for {} via {}: {}",
            host,
            fallback->get_host(),
            magic_enum::enum_name(result.status)
        );
    }

    return {};
}
}
