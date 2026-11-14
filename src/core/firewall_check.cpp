#include "firewall_check.hpp"

#ifdef _WIN32
#include <cwchar>
#include <string>

#include <windows.h>
#include <netfw.h>
#include <oleauto.h>
#include <spdlog/spdlog.h>

namespace core::firewall {
namespace {
// Defined here so no extra GUID libraries are needed.
constexpr CLSID CLSID_POLICY2{ 0xE2B3C97F, 0x6AE1, 0x41AC, { 0x81, 0x7A, 0xF6, 0xF9, 0x21, 0x66, 0xD7, 0xDD } };
constexpr IID IID_POLICY2{ 0x98325047, 0xC671, 0x4174, { 0x8D, 0x81, 0xDE, 0xFC, 0xD3, 0xF0, 0x31, 0x86 } };
constexpr IID IID_RULE{ 0xAF230D27, 0xBABA, 0x4E42, { 0xAC, 0xED, 0xF5, 0x24, 0xF2, 0x2C, 0xFC, 0xE2 } };
constexpr IID IID_ENUM_VARIANT{ 0x00020404, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };

constexpr long PROFILE_DOMAIN{ 1 };
constexpr long PROFILE_PRIVATE{ 2 };
constexpr long PROFILE_PUBLIC{ 4 };

template <typename T>
struct ComPtr {
    T* p{ nullptr };
    ~ComPtr() { if (p) { p->Release(); } }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

std::string utf8(const wchar_t* text)
{
    if (!text) {
        return {};
    }
    const int size{ WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr) };
    if (size <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring expand(const wchar_t* path)
{
    if (!path) {
        return {};
    }
    const DWORD size{ ExpandEnvironmentStringsW(path, nullptr, 0) };
    std::wstring out(size, L'\0');
    ExpandEnvironmentStringsW(path, out.data(), size);
    while (!out.empty() && out.back() == L'\0') {
        out.pop_back();
    }
    return out;
}

std::string profile_names(const long profiles)
{
    if (profiles == NET_FW_PROFILE2_ALL) {
        return "all";
    }
    std::string out;
    const auto add = [&out](const char* name) { out += out.empty() ? name : std::string{ "," } + name; };
    if (profiles & PROFILE_DOMAIN) { add("domain"); }
    if (profiles & PROFILE_PRIVATE) { add("private"); }
    if (profiles & PROFILE_PUBLIC) { add("public"); }
    return out.empty() ? "none" : out;
}

std::string protocol_name(const long protocol)
{
    switch (protocol) {
    case 6: return "TCP";
    case 17: return "UDP";
    case 256: return "any";
    default: return std::to_string(protocol);
    }
}

bool applies_to_udp(const long protocol)
{
    return protocol == 17 || protocol == 256;
}
}

void log_status()
{
    const HRESULT init{ CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED) };
    const bool uninitialize{ SUCCEEDED(init) };

    [&] {
        ComPtr<INetFwPolicy2> policy;
        if (FAILED(CoCreateInstance(CLSID_POLICY2, nullptr, CLSCTX_INPROC_SERVER, IID_POLICY2, reinterpret_cast<void**>(&policy)))) {
            spdlog::info("Firewall: could not query Windows Firewall (third-party firewall or service off?)");
            return;
        }

        long current{ 0 };
        if (FAILED(policy->get_CurrentProfileTypes(&current)) || (current & (PROFILE_DOMAIN | PROFILE_PRIVATE | PROFILE_PUBLIC)) == 0) {
            spdlog::info("Firewall: could not read the active network profile");
            return;
        }

        bool firewall_on{ false };
        for (const long profile : { PROFILE_DOMAIN, PROFILE_PRIVATE, PROFILE_PUBLIC }) {
            if (!(current & profile)) {
                continue;
            }
            VARIANT_BOOL enabled{ VARIANT_FALSE };
            VARIANT_BOOL block_all{ VARIANT_FALSE };
            policy->get_FirewallEnabled(static_cast<NET_FW_PROFILE_TYPE2>(profile), &enabled);
            policy->get_BlockAllInboundTraffic(static_cast<NET_FW_PROFILE_TYPE2>(profile), &block_all);
            firewall_on = firewall_on || enabled == VARIANT_TRUE;
            spdlog::info(
                "Firewall: active network profile {} - firewall {}{}",
                profile_names(profile),
                enabled == VARIANT_TRUE ? "on" : "off",
                block_all == VARIANT_TRUE ? ", BLOCK ALL INBOUND CONNECTIONS is enabled" : ""
            );
            if (enabled == VARIANT_TRUE && block_all == VARIANT_TRUE) {
                spdlog::error(
                    "Firewall: 'Block all incoming connections' is on for the {} profile; the Growtopia "
                    "server's replies will not reach the proxy. Turn that option off.",
                    profile_names(profile)
                );
            }
        }

        if (!firewall_on) {
            spdlog::info("Firewall: Windows Firewall is off for the active network; it cannot block the proxy");
            return;
        }

        wchar_t exe_path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe_path, MAX_PATH);

        ComPtr<INetFwRules> rules;
        ComPtr<IUnknown> unknown;
        ComPtr<IEnumVARIANT> enumerator;
        if (
            FAILED(policy->get_Rules(&rules)) || !rules
            || FAILED(rules->get__NewEnum(&unknown)) || !unknown
            || FAILED(unknown->QueryInterface(IID_ENUM_VARIANT, reinterpret_cast<void**>(&enumerator)))
        ) {
            spdlog::info("Firewall: could not list Windows Firewall rules");
            return;
        }

        int matching{ 0 };
        long udp_allowed_profiles{ 0 };
        long udp_blocked_profiles{ 0 };

        VARIANT item;
        VariantInit(&item);
        ULONG fetched{ 0 };
        while (enumerator->Next(1, &item, &fetched) == S_OK && fetched == 1) {
            ComPtr<INetFwRule> rule;
            if (item.vt == VT_DISPATCH && item.pdispVal
                && SUCCEEDED(item.pdispVal->QueryInterface(IID_RULE, reinterpret_cast<void**>(&rule)))) {
                BSTR application{ nullptr };
                rule->get_ApplicationName(&application);
                if (application && _wcsicmp(expand(application).c_str(), exe_path) == 0) {
                    BSTR name{ nullptr };
                    NET_FW_ACTION action{ NET_FW_ACTION_ALLOW };
                    NET_FW_RULE_DIRECTION direction{ NET_FW_RULE_DIR_IN };
                    VARIANT_BOOL enabled{ VARIANT_FALSE };
                    long protocol{ 0 };
                    long profiles{ 0 };
                    rule->get_Name(&name);
                    rule->get_Action(&action);
                    rule->get_Direction(&direction);
                    rule->get_Enabled(&enabled);
                    rule->get_Protocol(&protocol);
                    rule->get_Profiles(&profiles);

                    ++matching;
                    spdlog::info(
                        "Firewall rule '{}': {} {} {} for {} profile(s){}",
                        utf8(name),
                        action == NET_FW_ACTION_BLOCK ? "BLOCK" : "allow",
                        direction == NET_FW_RULE_DIR_IN ? "inbound" : "outbound",
                        protocol_name(protocol),
                        profile_names(profiles),
                        enabled == VARIANT_TRUE ? "" : " (disabled)"
                    );

                    if (enabled == VARIANT_TRUE && direction == NET_FW_RULE_DIR_IN && applies_to_udp(protocol)) {
                        (action == NET_FW_ACTION_BLOCK ? udp_blocked_profiles : udp_allowed_profiles) |= profiles;
                    }
                    SysFreeString(name);
                }
                SysFreeString(application);
            }
            VariantClear(&item);
        }

        if (udp_blocked_profiles & current) {
            spdlog::error(
                "Firewall: Windows Firewall BLOCKS inbound UDP for this Proxy.exe on the current network "
                "({}). The Growtopia server's replies cannot reach the proxy and the game will hang on "
                "\"Located server, connecting\". Allow Proxy.exe in Windows Firewall (see README-RU.txt).",
                profile_names(current)
            );
        } else if (!(udp_allowed_profiles & current)) {
            spdlog::warn(
                "Firewall: no rule allows inbound UDP for this Proxy.exe on the current network ({}){}. "
                "If the game hangs on \"Located server, connecting\", allow Proxy.exe in Windows Firewall.",
                profile_names(current),
                matching == 0 ? "; Windows has no rules for this exe yet" : ""
            );
        } else {
            spdlog::info("Firewall: inbound UDP allowed for this Proxy.exe on the current network");
        }
    }();

    if (uninitialize) {
        CoUninitialize();
    }
}
}
#else
namespace core::firewall {
void log_status() { }
}
#endif
