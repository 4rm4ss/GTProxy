#pragma once
#include <string>
#include <string_view>
#include <vector>

#include <glaze/glaze.hpp>
#include <spdlog/spdlog.h>

#include "dns_resolver.hpp"
#include "../utils/network.hpp"

namespace network {
struct DnsAnswer {
    int type{};
    std::string data;
};

struct DnsResponse {
    int status{};
    std::vector<DnsAnswer> answer;
};
}

template<> struct glz::meta<network::DnsResponse> {
    static constexpr std::string_view rename_key(const std::string_view key) {
        if (key == "status") {
            return "Status";
        }

        if (key == "answer") {
            return "Answer";
        }

        // ReSharper disable once CppDFALocalValueEscapesFunction
        return key;
    }
};

namespace network {
// Parses a DNS-over-HTTPS JSON response (application/dns-json) and returns the
// first A record. The answer may list CNAME records before the A record.
[[nodiscard]] inline DnsResult parse_dns_json(const std::string_view body)
{
    if (body.empty()) {
        return { DnsStatus::ServerFail, {} };
    }

    DnsResponse dns_response{};
    if (const auto ec = glz::read<glz::opts{ .error_on_unknown_keys = false }>(dns_response, body)) {
        spdlog::error("Failed to parse DNS response: {}", glz::format_error(ec, body));
        return { DnsStatus::ServerFail, {} };
    }

    const auto status{ static_cast<DnsStatus>(dns_response.status) };
    if (status != DnsStatus::NoError) {
        return { status, {} };
    }

    constexpr int DNS_TYPE_A{ 1 };
    for (const auto& answer : dns_response.answer) {
        if (answer.type == DNS_TYPE_A && utils::network::is_valid_ip_address(answer.data)) {
            return { status, answer.data };
        }
    }

    return { DnsStatus::NameError, {} };
}
}
