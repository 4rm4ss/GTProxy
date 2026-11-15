#include "trust_store.hpp"

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>

#include <fmt/format.h>

namespace core::trust_store {
namespace {
std::string describe(const DWORD error)
{
    if (error == ERROR_CANCELLED) {
        return "you pressed No in the Windows security warning";
    }
    char* text{ nullptr };
    const auto length{ FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<LPSTR>(&text), 0, nullptr
    ) };
    std::string message{ length > 0 && text ? std::string{ text, length } : std::string{} };
    if (text) {
        LocalFree(text);
    }
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == '.')) {
        message.pop_back();
    }
    return fmt::format("Windows error 0x{:08X}{}", static_cast<unsigned>(error), message.empty() ? "" : ": " + message);
}

HCERTSTORE open_user_root()
{
    return CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_CURRENT_USER, L"ROOT");
}

}

Result add_user_root(const std::span<const std::byte> der)
{
    const auto store{ open_user_root() };
    if (!store) {
        return { false, "cannot open the trusted root store: " + describe(GetLastError()) };
    }

    const BOOL added{ CertAddEncodedCertificateToStore(
        store,
        X509_ASN_ENCODING,
        reinterpret_cast<const BYTE*>(der.data()),
        static_cast<DWORD>(der.size()),
        CERT_STORE_ADD_REPLACE_EXISTING,
        nullptr
    ) };
    const DWORD error{ added ? 0 : GetLastError() };
    CertCloseStore(store, 0);
    if (!added) {
        return { false, describe(error) };
    }
    return { true, {} };
}

}
#else
namespace core::trust_store {
Result add_user_root(std::span<const std::byte>)
{
    return { false, "only on Windows" };
}
}
#endif
