#pragma once
#include <cstddef>
#include <span>
#include <string>

// Adding to the current user's trusted root certificates on Windows through
// the Windows certificate API (no certutil, no command line: folder names in
// any language work). Kept apart from the OpenSSL code: wincrypt.h and the
// OpenSSL headers define the same names.
namespace core::trust_store {
struct Result {
    bool ok{ false };
    std::string error;      // why it failed, for the log
};

// Adds a certificate (DER) to Current User\Trusted Root Certification
// Authorities. Windows asks you to confirm.
[[nodiscard]] Result add_user_root(std::span<const std::byte> der);
}
