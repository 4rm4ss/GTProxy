#pragma once
#include <filesystem>
#include <string>

namespace core::certificate {
// Common name of the local CA, also used to find it again in trust stores.
inline constexpr const char* CA_COMMON_NAME{ "GTProxy Local CA" };

struct Paths {
    std::filesystem::path ca_cert;      // trust this one (public)
    std::filesystem::path ca_key;       // never leaves this machine
    std::filesystem::path server_cert;  // leaf + CA chain served by the HTTPS server
    std::filesystem::path server_key;
};

[[nodiscard]] Paths paths_in(const std::filesystem::path& directory);

// Makes sure a CA unique to this machine and a server certificate for the
// Growtopia host names exist in `directory`, creating them on first run and
// renewing the server certificate when it is about to expire. The game only
// accepts the proxy after the CA is trusted (see install_ca()).
// Throws std::runtime_error on failure.
[[nodiscard]] Paths ensure(const std::filesystem::path& directory);

// Adds/removes the CA in the current user's trust store (Windows: certutil,
// asks for confirmation; macOS: login keychain). Returns false if it failed or
// the platform needs manual steps (which are logged).
bool install_ca(const Paths& paths);
bool uninstall_ca();
}
